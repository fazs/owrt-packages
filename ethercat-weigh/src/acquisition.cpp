#include "acquisition.hpp"

#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include "log.hpp"
#include "units.hpp"

namespace ew {

using clock_t_ = std::chrono::steady_clock;

namespace {

constexpr size_t kTareSamples = 50;  // Mittelung für Tara (bei 10 ms: 0,5 s)

nlohmann::json status_json(const El3356Status& s) {
    return {{"overrange", s.overrange},
            {"data_invalid", s.data_invalid},
            {"error", s.error},
            {"calibration_in_progress", s.calibration_in_progress},
            {"steady_state", s.steady_state},
            {"sync_error", s.sync_error}};
}

nlohmann::json window_json(const WindowStats& w) {
    return {{"n", w.n},         {"span_ms", w.span_ms}, {"mean", w.mean},
            {"range", w.range}, {"stddev", w.stddev}};
}

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

double num(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number())
        throw std::runtime_error(std::string("Feld '") + key + "' fehlt oder ist keine Zahl");
    return j[key].get<double>();
}

std::string str(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string())
        throw std::runtime_error(std::string("Feld '") + key + "' fehlt");
    return j[key].get<std::string>();
}

} // namespace

Acquisition::Acquisition(std::unique_ptr<IoBackend> backend, Config cfg, std::string config_path)
    : backend_(std::move(backend)),
      config_path_(std::move(config_path)),
      cfg_(cfg),
      cfg_saved_(cfg),
      char_(Characteristic::from_config(cfg.calibration, cfg.device)),
      engine_(cfg.measurement),
      stats_(cfg.statistics.rolling_window),
      cal_json_(cal_.to_json()) {}

Acquisition::~Acquisition() { stop(); }

void Acquisition::set_publisher(Publisher p) {
    std::lock_guard<std::mutex> lk(pub_mtx_);
    publisher_ = std::move(p);
}

void Acquisition::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void Acquisition::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    drain_requests(false);
    backend_->close();
}

void Acquisition::publish(const nlohmann::json& j) {
    Publisher p;
    {
        std::lock_guard<std::mutex> lk(pub_mtx_);
        p = publisher_;
    }
    if (p) p(j.dump());
}

// --- Request-Queue -------------------------------------------------------------

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

void Acquisition::drain_requests(bool backend_ok) {
    std::deque<std::shared_ptr<Request>> q;
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        q.swap(requests_);
    }
    for (auto& r : q) {
        CalResult res;
        if (!backend_ok) {
            res = {false, "Prozessdatenquelle nicht aktiv"};
        } else {
            try {
                res = r->fn();
            } catch (const std::exception& ex) {
                res = {false, ex.what()};
            }
        }
        r->result.set_value(res);
    }
}

CalResult Acquisition::tare() {
    return submit([this]() -> CalResult {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!sample_valid(last_) || gross_hist_.size() < kTareSamples / 2)
            return {false, "Tara nicht möglich: Messwert ungültig"};
        double sum = 0.0;
        for (double g : gross_hist_) sum += g;
        tare_g_ = sum / gross_hist_.size();
        engine_.reset();
        log_info("Tara %.4f g", *tare_g_);
        return {true, {}};
    });
}

CalResult Acquisition::clear_tare() {
    return submit([this]() -> CalResult {
        std::lock_guard<std::mutex> lk(mtx_);
        tare_g_.reset();
        engine_.reset();
        return {true, {}};
    });
}

CalResult Acquisition::set_sim_load(std::optional<double> grams) {
    return submit([this, grams]() -> CalResult {
        if (!backend_->set_sim_load(grams)) return {false, "Nur im Simulationsbetrieb verfügbar"};
        return {true, {}};
    });
}

CalResult Acquisition::calibration(const nlohmann::json& request) {
    return submit([this, request]() { return handle_calibration(request, now_); }, 5000);
}

// Läuft im Acquisition-Thread
CalResult Acquisition::handle_calibration(const nlohmann::json& req, double t) {
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
            } catch (const std::exception& ex) {
                return {false, ex.what()};
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                apply_new_config_locked(next);
                device_pending_ = false;  // Klemme ist bereits auf dem Stand der Kalibrierung
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

nlohmann::json Acquisition::measurement_json(const Measurement& m) const {
    return {{"id", m.id},       {"t", m.t}, {"value", m.value}, {"stddev", m.stddev},
            {"range", m.range}, {"n", m.n}, {"settle_ms", m.settle_ms}};
}

nlohmann::json Acquisition::sample_json_locked() const {
    return {{"type", "sample"},
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
            {"calibration", cal_json_}};
}

nlohmann::json Acquisition::info() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return {{"version", EW_VERSION},
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
            {"max_cycle_ms", max_cycle_ms_}};
}

nlohmann::json Acquisition::snapshot() const {
    nlohmann::json j;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        j["sample"] = sample_json_locked();
        j["statistics"] = stats_.to_json();
        nlohmann::json hist = nlohmann::json::array();
        const size_t n = history_.size();
        for (size_t i = n > 50 ? n - 50 : 0; i < n; ++i) hist.push_back(measurement_json(history_[i]));
        j["measurements"] = hist;
    }
    j["info"] = info();
    return j;
}

nlohmann::json Acquisition::measurements_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json a = nlohmann::json::array();
    for (const auto& m : history_) a.push_back(measurement_json(m));
    return a;
}

std::string Acquisition::measurements_csv(const std::string& unit) const {
    const std::string u = valid_unit(unit) ? unit : "g";
    std::lock_guard<std::mutex> lk(mtx_);
    std::ostringstream os;
    os.precision(9);
    os << "id;t_s;value_" << u << ";stddev_" << u << ";range_" << u << ";n;settle_ms\n";
    for (const auto& m : history_)
        os << m.id << ';' << m.t << ';' << from_grams(m.value, u) << ';'
           << from_grams(m.stddev, u) << ';' << from_grams(m.range, u) << ';' << m.n << ';'
           << m.settle_ms << '\n';
    return os.str();
}

Config Acquisition::config() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_saved_;
}

// --- Konfiguration -----------------------------------------------------------

void Acquisition::apply_new_config_locked(const Config& next) {
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

ConfigUpdateResult Acquisition::update_config(const Config& next) {
    ConfigUpdateResult r;
    r.errors = next.validate();
    if (!r.errors.empty()) return r;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cal_json_.value("active", false)) {
            r.errors.push_back("Kalibrierung aktiv: Parameter erst danach ändern");
            return r;
        }
    }
    try {
        save_config(config_path_, next);
    } catch (const std::exception& ex) {
        r.errors.push_back(ex.what());
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

// --- Zyklus --------------------------------------------------------------------

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
            for (int i = 0; i < 20 && running_; ++i) {
                drain_requests(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
                    stat_msg = {{"type", "measurement"},
                                {"measurement", measurement_json(*m)},
                                {"statistics", stats_.to_json()}};
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
