#include "acquisition.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

#include "log.hpp"
#include "measurement_csv.hpp"
#include "units.hpp"

namespace ew {
    using clock_t_ = std::chrono::steady_clock;

    namespace {
        constexpr size_t kTareSamples = 50U; ///< Mittelung für Tara (bei 10 ms: 0,5 s)
        constexpr size_t kMinTareSamples = kTareSamples / 2U; ///< Mindestanzahl gültiger Werte für Tara
        constexpr int kCalibrationTimeoutMs = 5000; ///< Kalibrierkommandos (SDO-Zugriffe) [ms]
        constexpr size_t kSdoReadBuffer = 64U; ///< Debug: Puffer für SDO-Lesezugriffe (> Objektgröße)
        constexpr size_t kSnapshotMeasurements = 50U; ///< Messungen im Snapshot (/api/state, init)
        constexpr int kRetryDelayMs = 100; ///< Wartezeit zwischen Prüfungen bei Backend-Fehler [ms]
        constexpr int kRetrySteps = 20; ///< Prüfungen bis zum nächsten Öffnungsversuch (2 s)
        constexpr unsigned kNibbleBits = 4U; ///< Bits einer Hex-Ziffer
        constexpr uint8_t kNibbleMask = 0x0FU; ///< Maske einer Hex-Ziffer

        /// @brief Statusbits als JSON für das Frontend.
        nlohmann::json status_json(const El3356Status &s) {
            return {
                {"overrange", s.overrange},
                {"data_invalid", s.data_invalid},
                {"error", s.error},
                {"calibration_in_progress", s.calibration_in_progress},
                {"steady_state", s.steady_state},
                {"sync_error", s.sync_error}
            };
        }

        /// @brief Kennzahlen des Stabilitätsfensters als JSON.
        nlohmann::json window_json(const WindowStats &w) {
            return {
                {"n", w.n},
                {"span_ms", w.span_ms},
                {"mean", w.mean},
                {"range", w.range},
                {"stddev", w.stddev}
            };
        }

        /**
         * @brief Setzt SCHED_FIFO für den aufrufenden Thread.
         * @param prio 1..99; <= 0 lässt SCHED_OTHER
         */
        void set_rt_priority(int prio) {
            if (prio <= 0) return;
            sched_param sp{};
            sp.sched_priority = prio;
            const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
            if (rc != 0)
                log_warn("SCHED_FIFO %d nicht gesetzt: %s", prio, std::strerror(rc));
            else
                log_info("Acquisition-Thread: SCHED_FIFO %d", prio);
        }

        /**
         * @brief Pflichtfeld als Zahl.
         * @throws std::runtime_error wenn das Feld fehlt oder keine Zahl ist
         */
        double num(const nlohmann::json &j, const char *key) {
            if (!j.contains(key) || !j[key].is_number())
                throw std::runtime_error(std::string("Feld '") + key + "' fehlt oder ist keine Zahl");
            return j[key].get<double>();
        }

        /// @brief Bytes als Hex-Text ("01 A0 ...").
        std::string hex(const std::vector<uint8_t> &d) {
            static const char *digits = "0123456789ABCDEF";
            std::string out;
            for (size_t i = 0; i < d.size(); ++i) {
                if (i) out += ' ';
                out += digits[d[i] >> kNibbleBits];
                out += digits[d[i] & kNibbleMask];
            }
            return out;
        }

        /// @brief Wert als JSON gemäß Typ: bool, Ganzzahl oder Gleitkomma.
        nlohmann::json typed(el3356::DType t, double v) {
            if (t == el3356::DType::Bool) return v != 0.0;
            if (t == el3356::DType::F32) return v;
            if (t == el3356::DType::U64) return static_cast<uint64_t>(v);
            return static_cast<int64_t>(v);
        }

        /**
         * @brief Pflichtfeld als Text.
         * @throws std::runtime_error wenn das Feld fehlt oder kein Text ist
         */
        std::string str(const nlohmann::json &j, const char *key) {
            if (!j.contains(key) || !j[key].is_string())
                throw std::runtime_error(std::string("Feld '") + key + "' fehlt");
            return j[key].get<std::string>();
        }
    } // namespace

    /**
     * @param backend Prozessdatenquelle (EtherCAT oder Simulation)
     * @param cfg Startkonfiguration
     * @param config_path Pfad, unter dem Änderungen gespeichert werden
     */
    Acquisition::Acquisition(std::unique_ptr<IoBackend> backend, Config cfg, std::string config_path)
        : backend_(std::move(backend)),
          config_path_(std::move(config_path)),
          cfg_(cfg),
          cfg_saved_(cfg),
          char_(Characteristic::from_config(cfg.calibration, cfg.device)),
          engine_(cfg.measurement),
          stats_(cfg.statistics.rolling_window),
          cal_json_(cal_.to_json()) {
    }

    /// @brief Stoppt den Thread und schließt die Prozessdatenquelle.
    Acquisition::~Acquisition() { stop(); }

    /// @brief Setzt den Empfänger der JSON-Ereignisse (nullptr = keiner).
    void Acquisition::set_publisher(Publisher p) {
        std::lock_guard<std::mutex> lk(pub_mtx_);
        publisher_ = std::move(p);
    }

    /// @brief Startet den Acquisition-Thread (mehrfacher Aufruf ohne Wirkung).
    void Acquisition::start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }

    /// @brief Stoppt den Thread, beantwortet offene Kommandos mit Fehler und schließt die Quelle.
    void Acquisition::stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        drain_requests(false);
        backend_->close();
    }

    /// @brief Verteilt ein Ereignis an den Publisher.
    void Acquisition::publish(const nlohmann::json &j) {
        Publisher p;
        {
            std::lock_guard<std::mutex> lk(pub_mtx_);
            p = publisher_;
        }
        if (p) p(j.dump());
    }

    // --- Request-Queue -------------------------------------------------------------

    /**
     * @brief Führt @p fn im Acquisition-Thread aus und wartet auf das Ergebnis.
     * @param fn Kommando
     * @param timeout_ms maximale Wartezeit
     * @return Ergebnis von @p fn oder Fehler bei Zeitüberschreitung
     */
    CalResult Acquisition::submit(std::function<CalResult()> fn, int timeout_ms) {
        auto r = std::make_shared<Request>();
        r->fn = std::move(fn);
        auto fut = r->result.get_future();
        {
            std::lock_guard<std::mutex> lk(req_mtx_);
            requests_.push_back(r);
        }
        if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
            return {false, "Zeitüberschreitung bei der Ausführung"};
        return fut.get();
    }

    /**
     * @brief Wie submit(), aber mit JSON-Ergebnis.
     * @param fn Abfrage
     * @param timeout_ms maximale Wartezeit
     * @return Ergebnis von @p fn oder {"ok": false, "error": ...}
     */
    nlohmann::json Acquisition::submit_json(std::function<nlohmann::json()> fn, int timeout_ms) {
        auto out = std::make_shared<nlohmann::json>(); // überlebt eine Zeitüberschreitung
        const auto r = submit(
            [out, fn = std::move(fn)]() -> CalResult {
                *out = fn();
                return {true, {}};
            },
            timeout_ms);
        if (!r.ok) return {{"ok", false}, {"error", r.error}};
        return *out;
    }

    /**
     * @brief Arbeitet alle wartenden Kommandos ab.
     * @param backend_ok false: Kommandos mit "Prozessdatenquelle nicht aktiv" beantworten
     */
    void Acquisition::drain_requests(bool backend_ok) {
        std::deque<std::shared_ptr<Request> > q;
        {
            std::lock_guard<std::mutex> lk(req_mtx_);
            q.swap(requests_);
        }
        for (auto &r: q) {
            CalResult res;
            if (!backend_ok) {
                res = {false, "Prozessdatenquelle nicht aktiv"};
            } else {
                try {
                    res = r->fn();
                } catch (const std::exception &ex) {
                    res = {false, ex.what()};
                }
            }
            r->result.set_value(res);
        }
    }

    /**
     * @brief Setzt Tara aus dem Mittel der letzten gültigen Bruttowerte.
     * @return Fehler, wenn der Messwert ungültig ist oder zu wenige Werte vorliegen
     */
    CalResult Acquisition::tare() {
        return submit([this]() -> CalResult {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!sample_valid(last_) || gross_hist_.size() < kMinTareSamples)
                return {false, "Tara nicht möglich: Messwert ungültig"};
            double sum = 0.0;
            for (double g: gross_hist_) sum += g;
            tare_g_ = sum / static_cast<double>(gross_hist_.size());
            engine_.reset();
            log_info("Tara %.4f g", *tare_g_);
            return {true, {}};
        });
    }

    /// @brief Löscht Tara.
    CalResult Acquisition::clear_tare() {
        return submit([this]() -> CalResult {
            std::lock_guard<std::mutex> lk(mtx_);
            tare_g_.reset();
            engine_.reset();
            return {true, {}};
        });
    }

    /**
     * @brief Nur Simulation: aufgelegte Masse vorgeben.
     * @param grams Masse [g], std::nullopt = automatisches Lastprofil
     */
    CalResult Acquisition::set_sim_load(std::optional<double> grams) {
        return submit([this, grams]() -> CalResult {
            if (!backend_->set_sim_load(grams)) return {false, "Nur im Simulationsbetrieb verfügbar"};
            return {true, {}};
        });
    }

    /**
     * @brief Kalibrierkommando aus der REST-API (siehe README).
     * @param request {"action": ..., weitere Felder je Aktion}
     */
    CalResult Acquisition::calibration(const nlohmann::json &request) {
        return submit([this, request]() { return handle_calibration(request, now_); }, kCalibrationTimeoutMs);
    }

    // --- Debug -------------------------------------------------------------------------

    /**
     * @brief Debug: PDO-Katalog mit aktuellen Werten aus dem Prozessabbild.
     * @return {"ok", "pdos": [...], "inputs", "outputs"} (Abbild als Hex)
     */
    nlohmann::json Acquisition::debug_pdos() {
        return submit_json([this] {
            std::vector<uint8_t> in, out;
            backend_->process_image(in, out);
            nlohmann::json pdos = nlohmann::json::array();
            for (const auto &p: backend_->pdos()) {
                const auto &img = p.rx ? out : in;
                nlohmann::json entries = nlohmann::json::array();
                for (const auto &e: p.entries) {
                    const bool in_img = e.loc.present && e.type != el3356::DType::None &&
                                        el3356::bytes_for_bits(e.loc.bit_offset + e.loc.bit_len) <= img.size();
                    nlohmann::json ov = nullptr;
                    if (p.rx && e.map.index == el3356::kObjControl)
                        if (auto it = ctrl_override_.find(e.map.sub); it != ctrl_override_.end())
                            ov = it->second;
                    entries.push_back(
                        {
                            {"index", e.map.index},
                            {"sub", e.map.sub},
                            {"bits", e.map.bits},
                            {"name", e.name},
                            {"type", el3356::dtype_name(e.type)},
                            {"offset", e.loc.present ? nlohmann::json(e.loc.bit_offset) : nlohmann::json(nullptr)},
                            {
                                "value", in_img
                                             ? typed(e.type, el3356::entry_value(img.data(), e))
                                             : nlohmann::json(nullptr)
                            },
                            {"override", ov}
                        });
                }
                pdos.push_back({
                    {"index", p.index},
                    {"name", p.name},
                    {"rx", p.rx},
                    {"assigned", p.assigned},
                    {"excludes", p.excludes},
                    {"entries", entries}
                });
            }
            return nlohmann::json{{"ok", true}, {"pdos", pdos}, {"inputs", hex(in)}, {"outputs", hex(out)}};
        });
    }

    /**
     * @brief Debug: übersteuert ein Steuerbit 0x7000 im Prozessabbild.
     * @param sub Subindex 1..5; std::nullopt hebt alle Übersteuerungen auf
     * @param value Wert; std::nullopt hebt die Übersteuerung von @p sub auf
     */
    CalResult Acquisition::debug_override(std::optional<uint8_t> sub, std::optional<bool> value) {
        if (sub.has_value()) {
            El3356Control probe;
            if (!el3356::control_bit(probe, *sub)) return {false, "Kein Steuerbit 0x7000:" + std::to_string(*sub)};
        }
        return submit([this, sub, value]() -> CalResult {
            if (!sub)
                ctrl_override_.clear();
            else if (value)
                ctrl_override_[*sub] = *value;
            else
                ctrl_override_.erase(*sub);
            return {true, {}};
        });
    }

    /**
     * @brief Debug: einzelner SDO-Zugriff.
     * @param req {"op": "read"|"write", "index", "sub", "type", "value"}
     * @return {"ok", "value", "hex"} bzw. {"ok": false, "error"}
     * @throws std::runtime_error bei ungültiger Anfrage
     */
    nlohmann::json Acquisition::debug_sdo(const nlohmann::json &req) {
        const std::string op = str(req, "op");
        const double index = num(req, "index"), sub = num(req, "sub");
        if (index < 0 || index > std::numeric_limits<uint16_t>::max() || sub < 0 ||
            sub > std::numeric_limits<uint8_t>::max())
            throw std::runtime_error("Index oder Subindex außerhalb des Wertebereichs");
        el3356::DType type;
        if (!el3356::dtype_from_name(str(req, "type"), type)) throw std::runtime_error("Unbekannter Datentyp");
        if (op != "read" && op != "write") throw std::runtime_error("op muss read oder write sein");
        std::vector<uint8_t> data;
        if (op == "write") {
            if (!req.contains("value") || !(req["value"].is_number() || req["value"].is_boolean()))
                throw std::runtime_error("Feld 'value' fehlt");
            data = el3356::encode_value(type, req["value"].is_boolean()
                                                  ? req["value"].get<bool>()
                                                  : req["value"].get<double>());
        }
        const auto idx = static_cast<uint16_t>(index);
        const auto si = static_cast<uint8_t>(sub);
        return submit_json([this, op, idx, si, type, data]() mutable {
            std::string err;
            bool ok;
            if (op == "write") {
                ok = backend_->sdo_write(idx, si, data, err);
            } else {
                ok = backend_->sdo_read(idx, si, kSdoReadBuffer, data, err);
            }
            if (!ok) return nlohmann::json{{"ok", false}, {"error", err}};
            log_info("Debug: SDO-%s 0x%04X:%02X = %s", op == "write" ? "Write" : "Read", idx, si,
                     hex(data).c_str());
            return nlohmann::json{
                {"ok", true},
                {"value", typed(type, el3356::decode_value(type, data))},
                {"hex", hex(data)}
            };
        });
    }

    /**
     * @brief Debug: Initialisierungssequenz mit den Werten der aktuellen Konfiguration.
     * @return {"steps": [{index, sub, name, type, value, hex, pre_op_only}]}
     */
    nlohmann::json Acquisition::debug_init_sequence() const {
        nlohmann::json steps = nlohmann::json::array();
        for (const auto &w: el3356::init_sequence(config().device))
            steps.push_back({
                {"index", w.index},
                {"sub", w.sub},
                {"name", w.name},
                {"type", el3356::dtype_name(w.type)},
                {"value", typed(w.type, el3356::decode_value(w.type, w.data))},
                {"hex", hex(w.data)},
                {"pre_op_only", w.pre_op_only}
            });
        return {{"steps", steps}};
    }

    /// @brief Schließt die Prozessdatenquelle und öffnet sie neu (INIT -> OP inkl. Initialisierungssequenz).
    void Acquisition::reinit() {
        log_info("Neuinitialisierung angefordert");
        reinit_ = true;
    }

    /**
     * @brief Führt ein Kalibrierkommando aus (im Acquisition-Thread).
     * @param req Anfrage mit "action"
     * @param t aktuelle Zeit [s]
     * @return Ergebnis; bei "commit" wird die neue Konfiguration gespeichert und veröffentlicht
     */
    CalResult Acquisition::handle_calibration(const nlohmann::json &req, double t) {
        const std::string action = str(req, "action");
        Config cfg;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cfg = cfg_saved_;
        }
        CalResult r;
        if (action == "start_application") {
            r = cal_.start_application(cfg);
        } else if (action == "capture") {
            const std::string unit = str(req, "unit");
            if (!valid_unit(unit)) return {false, "Einheit muss mg, g, kg oder gr sein"};
            r = cal_.capture(to_grams(num(req, "mass"), unit), t);
        } else if (action == "remove_point") {
            const double idx = num(req, "index");
            if (idx < 0) return {false, "Index ungültig"};
            r = cal_.remove_point(static_cast<size_t>(idx));
        } else if (action == "start_device") {
            r = cal_.start_device(cfg, num(req, "nominal_load"), str(req, "unit"), *backend_);
        } else if (action == "zero") {
            r = cal_.device_zero(*backend_, t);
        } else if (action == "reference") {
            const std::string unit = str(req, "unit");
            if (!valid_unit(unit)) return {false, "Einheit muss mg, g, kg oder gr sein"};
            r = cal_.device_reference(to_grams(num(req, "mass"), unit), *backend_, t);
        } else if (action == "commit") {
            Config next;
            r = cal_.commit(cfg, next);
            if (r.ok) {
                try {
                    save_config(config_path_, next);
                } catch (const std::exception &ex) {
                    return {false, ex.what()};
                }
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    apply_new_config_locked(next);
                    device_pending_ = false; // Klemme ist bereits auf dem Stand der Kalibrierung
                }
                log_info("Kalibrierung übernommen (%s)", next.calibration.mode.c_str());
                publish({{"type", "config"}, {"config", next}, {"restart_required", false}});
            }
        } else if (action == "cancel") {
            cal_.cancel(cfg, *backend_);
            r = {true, {}};
        } else {
            return {false, "Unbekannte Aktion: " + action};
        }
        nlohmann::json cj = cal_.to_json();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cal_json_ = cj;
        }
        publish({{"type", "calibration"}, {"calibration", cj}});
        return r;
    }

    // --- Abfragen ------------------------------------------------------------------

    /// @brief Letztes Sample, Statistik, letzte 50 Messungen und info().
    nlohmann::json Acquisition::snapshot() const {
        nlohmann::json j;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            j["sample"] = sample_json_locked();
            j["statistics"] = stats_.to_json();
            nlohmann::json hist = nlohmann::json::array();
            const size_t n = history_.size();
            for (size_t i = n > kSnapshotMeasurements ? n - kSnapshotMeasurements : 0U; i < n; ++i)
                hist.push_back(measurement_json(history_[i]));
            j["measurements"] = hist;
        }
        j["info"] = info();
        return j;
    }

    /// @brief Alle gehaltenen Messungen als JSON (Massen in Gramm).
    nlohmann::json Acquisition::measurements_json() const {
        std::lock_guard<std::mutex> lk(mtx_);
        nlohmann::json a = nlohmann::json::array();
        for (const auto &m: history_) a.push_back(measurement_json(m));
        return a;
    }

    /**
     * @brief Alle gehaltenen Messungen als CSV (Trennzeichen ';').
     * @param unit Einheit der Massen; ungültig = g
     */
    std::string Acquisition::measurements_csv(const std::string &unit) const {
        const std::string u = valid_unit(unit) ? unit : "g";
        std::lock_guard<std::mutex> lk(mtx_);
        std::string csv = measurements_csv_header(u);
        for (const auto &m: history_) csv += measurement_csv_row(m, u);
        return csv;
    }

    /**
     * @brief Übernimmt Messungen, als wären sie gerade gemessen worden (z. B. aus --sim-csv).
     *
     * Die Messungen bekommen fortlaufende Nummern und gehen in Statistik und gehaltene Messungen
     * ein (höchstens statistics.history_size, die letzten der Liste). Die Zeitabstände bleiben
     * erhalten; die letzte Messung liegt auf dem aktuellen Zeitpunkt.
     *
     * Vor start() aufrufen (liest die Zykluszeit des Threads).
     *
     * @param ms Messungen in zeitlicher Reihenfolge (Massen in Gramm)
     * @return Anzahl übernommener Messungen
     */
    size_t Acquisition::import_measurements(const std::vector<Measurement> &ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        const size_t keep = std::min<size_t>(ms.size(), cfg_.statistics.history_size);
        const auto first = ms.end() - static_cast<std::ptrdiff_t>(keep);
        const double shift = keep > 0U ? now_ - ms.back().t : 0.0;
        for (auto it = first; it != ms.end(); ++it) {
            Measurement m = *it;
            m.id = engine_.get_next_id();
            m.t += shift;
            stats_.add(m.value);
            history_.push_back(m);
        }
        while (history_.size() > cfg_.statistics.history_size) history_.pop_front();
        return keep;
    }

    /// @brief Gespeicherte Konfiguration (inkl. Änderungen, die einen Neustart erfordern).
    Config Acquisition::config() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return cfg_saved_;
    }

    /// @brief Version, Backend-Zustand, Zyklen, Überläufe, max. Zykluszeit.
    nlohmann::json Acquisition::info() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return {
            {"version", EW_VERSION},
            {"backend", backend_->name()},
            {"backend_state", backend_state_},
            {"backend_error", backend_error_},
            {"interface", cfg_.ethercat.interface},
            {"cycle_ms", cfg_.ethercat.cycle_ms},
            {"calibration_mode", cfg_.calibration.mode},
            {"calibrated", char_.valid()},
            {"restart_required", restart_required_},
            {"cycles", cycles_},
            {"overruns", overruns_},
            {"max_cycle_ms", max_cycle_ms_}
        };
    }

    /// @brief Letztes Sample als "sample"-Nachricht; mtx_ muss gehalten werden.
    nlohmann::json Acquisition::sample_json_locked() const {
        return {
            {"type", "sample"},
            {"t", last_.t},
            {"value", last_.value},
            {"gross", last_.gross},
            {"raw", last_.raw},
            {"tare", tare_g_ ? nlohmann::json(*tare_g_) : nlohmann::json(nullptr)},
            {"valid", sample_valid(last_)},
            {"link_ok", last_.link_ok},
            {"wkc_ok", last_.wkc_ok},
            {"status", status_json(last_.status)},
            {"state", to_string(engine_.state())},
            {"fault", engine_.fault_reason()},
            {"armed", engine_.armed()},
            {"window", window_json(engine_.window())},
            {"calibration", cal_json_}
        };
    }

    /// @brief Messung als JSON.
    nlohmann::json Acquisition::measurement_json(const Measurement &m) const {
        return {
            {"id", m.id}, {"t", m.t}, {"value", m.value}, {"stddev", m.stddev},
            {"range", m.range}, {"n", m.n}, {"settle_ms", m.settle_ms}
        };
    }

    // --- Konfiguration -----------------------------------------------------------

    /**
     * @brief Übernimmt eine neue Konfiguration; mtx_ muss gehalten werden.
     *
     * ethercat/http wirken erst nach Neustart. Ändert sich die Kennlinie, werden Tara und
     * Stabilitätsfenster verworfen.
     *
     * @param next neue Konfiguration
     */
    void Acquisition::apply_new_config_locked(const Config &next) {
        // ethercat/http wirken erst nach Neustart; alles andere sofort
        Config applied = next;
        applied.ethercat = cfg_.ethercat;
        applied.http = cfg_.http;
        const bool restart = requires_restart(cfg_, next);
        restart_required_ = restart_required_ || restart;
        device_pending_ = device_pending_ || device_config_changed(cfg_, applied);
        const bool char_changed =
                nlohmann::json(cfg_)["calibration"] != nlohmann::json(applied)["calibration"] ||
                cfg_.device.scale_factor != applied.device.scale_factor;
        cfg_ = applied;
        cfg_saved_ = next;
        cfg_pending_ = true;
        char_ = Characteristic::from_config(cfg_.calibration, cfg_.device);
        if (char_changed) {
            tare_g_.reset();
            gross_hist_.clear();
            engine_.reset();
        }
    }

    /**
     * @brief Validiert, speichert und übernimmt eine neue Konfiguration.
     * @param next neue Konfiguration
     * @return Ergebnis; abgelehnt während einer laufenden Kalibrierung
     */
    ConfigUpdateResult Acquisition::update_config(const Config &next) {
        ConfigUpdateResult r;
        r.errors = next.validate();
        if (!r.errors.empty()) return r;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (cal_json_.value("active", false)) {
                r.errors.emplace_back("Kalibrierung aktiv: Parameter erst danach ändern");
                return r;
            }
        }
        try {
            save_config(config_path_, next);
        } catch (const std::exception &ex) {
            r.errors.emplace_back(ex.what());
            return r;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            r.restart_required = requires_restart(cfg_, next);
            apply_new_config_locked(next);
        }
        r.ok = true;
        publish({{"type", "config"}, {"config", next}, {"restart_required", r.restart_required}});
        return r;
    }

    /// @brief Setzt die Statistik zurück und löscht alle gehaltenen Messungen.
    void Acquisition::reset_statistics() {
        nlohmann::json st;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stats_.reset();
            history_.clear();
            st = stats_.to_json();
        }
        publish({{"type", "statistics_reset"}, {"statistics", st}});
    }

    /**
     * @brief Löscht eine gehaltene Messung und führt die Statistik nach.
     * @param id Nummer der Messung
     * @return false, wenn die Messung nicht (mehr) gehalten wird
     */
    bool Acquisition::delete_measurement(uint64_t id) {
        nlohmann::json st;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            const auto it = std::find_if(history_.begin(), history_.end(),
                                         [id](const Measurement &m) { return m.id == id; });
            if (it == history_.end()) return false;
            const double x = it->value;
            history_.erase(it);
            std::vector<double> held;
            held.reserve(history_.size());
            for (const auto &m: history_) held.push_back(m.value);
            stats_.remove(x, held);
            st = stats_.to_json();
        }
        log_info("Messung %llu gelöscht", static_cast<unsigned long long>(id));
        publish({{"type", "measurement_deleted"}, {"id", id}, {"statistics", st}});
        return true;
    }

    // --- Zyklus --------------------------------------------------------------------

    /**
     * @brief Thread-Funktion: Initialisierung mit Wiederholung, danach zyklischer Betrieb.
     *
     * Je Zyklus: Konfigurationsänderungen übernehmen, Prozessdaten, Kalibrierung, Kennlinie,
     * Tara, Messung, Statistik, Ereignisse, Kommandos.
     */
    void Acquisition::run() {
        set_rt_priority(config().ethercat.rt_priority);
        const auto t0 = clock_t_::now();
        auto secs = [&] { return std::chrono::duration<double>(clock_t_::now() - t0).count(); };

        while (running_) {
            // --- Initialisierung --------------------------------------------------
            std::string err;
            Config c;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                c = cfg_;
                backend_state_ = "init";
            }
            publish({{"type", "backend"}, {"state", "init"}, {"error", ""}});
            reinit_ = false;
            if (!backend_->open(c, err)) {
                log_error("Backend %s: %s", backend_->name().c_str(), err.c_str());
                backend_->close();
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    backend_state_ = "error";
                    backend_error_ = err;
                    last_ = {};
                    last_.t = secs();
                    engine_.process(last_);
                }
                publish({{"type", "backend"}, {"state", "error"}, {"error", err}});
                for (int i = 0; i < kRetrySteps && running_; ++i) {
                    drain_requests(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                backend_state_ = "op";
                backend_error_.clear();
            }
            publish({{"type", "backend"}, {"state", "op"}, {"error", ""}});
            log_info("Backend %s aktiv, Zyklus %u ms", backend_->name().c_str(), c.ethercat.cycle_ms);

            // --- Zyklischer Betrieb ----------------------------------------------
            const auto period = std::chrono::milliseconds(c.ethercat.cycle_ms);
            auto next = clock_t_::now();
            double last_ui = -1.0;

            while (running_) {
                if (reinit_) {
                    drain_requests(false);
                    backend_->close();
                    break;
                }
                bool apply_device = false;
                DeviceConfig dc;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (cfg_pending_) {
                        engine_.set_params(cfg_.measurement);
                        stats_.set_rolling_window(cfg_.statistics.rolling_window);
                        while (history_.size() > cfg_.statistics.history_size) history_.pop_front();
                        c = cfg_;
                        cfg_pending_ = false;
                    }
                    if (device_pending_) {
                        apply_device = true;
                        dc = cfg_.device;
                        device_pending_ = false;
                    }
                }
                if (apply_device) {
                    std::string derr;
                    const bool ok = backend_->write_device_config(dc, derr);
                    if (!ok) log_error("CoE-Parameter: %s", derr.c_str());
                    else log_info("CoE-Parameter 0x8000 geschrieben");
                    publish({{"type", "device_config"}, {"ok", ok}, {"error", derr}});
                }

                const auto cyc_start = clock_t_::now();
                const double t = secs();
                now_ = t;

                El3356Control ctrl;
                for (const auto &[sub, v]: ctrl_override_) *el3356::control_bit(ctrl, sub) = v;
                Sample s;
                backend_->cycle(t, ctrl, s);

                // Kalibrierablauf (SDO-Zugriffe ohne Lock)
                const bool cal_was_active = cal_.active();
                cal_.step(s, *backend_, c);
                nlohmann::json cal_j;
                if (cal_was_active || cal_.active()) cal_j = cal_.to_json();

                std::optional<Measurement> m;
                nlohmann::json ui_msg, stat_msg;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (!cal_j.is_null()) cal_json_ = cal_j;
                    s.calibrating = cal_.active();
                    const auto g = char_.to_grams(s.raw);
                    s.calibrated = g.has_value();
                    s.gross = g.value_or(0.0);
                    s.value = s.gross - tare_g_.value_or(0.0);
                    if (sample_valid(s)) {
                        gross_hist_.push_back(s.gross);
                        while (gross_hist_.size() > kTareSamples) gross_hist_.pop_front();
                    } else {
                        gross_hist_.clear();
                    }
                    last_ = s;
                    m = engine_.process(s);
                    if (m) {
                        stats_.add(m->value);
                        history_.push_back(*m);
                        while (history_.size() > c.statistics.history_size) history_.pop_front();
                        stat_msg = {
                            {"type", "measurement"},
                            {"measurement", measurement_json(*m)},
                            {"statistics", stats_.to_json()}
                        };
                    }
                    if (last_ui < 0.0 || t - last_ui >= 1.0 / c.ui.rate_hz) {
                        ui_msg = sample_json_locked();
                        last_ui = t;
                    }
                    ++cycles_;
                }
                if (!stat_msg.is_null()) publish(stat_msg);
                if (!ui_msg.is_null()) publish(ui_msg);

                drain_requests(true);

                const double exec_ms =
                        std::chrono::duration<double, std::milli>(clock_t_::now() - cyc_start).count();
                next += period;
                const auto now = clock_t_::now();
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (exec_ms > max_cycle_ms_) max_cycle_ms_ = exec_ms;
                    if (now > next + period) {
                        ++overruns_;
                        next = now;
                    }
                }
                std::this_thread::sleep_until(next);
            }
        }
    }
} // namespace ew
