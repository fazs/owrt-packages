#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "calibration.hpp"
#include "calibration_controller.hpp"
#include "config.hpp"
#include "device_commands.hpp"
#include "io_backend.hpp"
#include "measurement.hpp"
#include "statistics.hpp"

namespace ew {
    /// Ergebnis von Acquisition::update_config().
    struct ConfigUpdateResult {
        bool ok = false; ///< gespeichert und angewendet
        bool restart_required = false; ///< ethercat/http geändert, wirkt erst nach Neustart
        std::vector<std::string> errors; ///< Validierungs- bzw. Speicherfehler
    };

    /**
     * @brief Zyklischer Acquisition-Thread.
     *
     * Prozessdaten -> Kennlinie -> Tara -> MeasurementEngine -> TemporaryStatistics.
     * Ereignisse werden als JSON über den Publisher verteilt. Kommandos aus dem Web-Server
     * (Tara, Kalibrierung, Debug) werden über eine Queue im Acquisition-Thread ausgeführt,
     * da SOEM nicht threadsicher ist.
     */
    class Acquisition {
    public:
        /// Empfänger der JSON-Ereignisse (WebSocket-Broadcast)
        using Publisher = std::function<void(const std::string &)>;

        Acquisition(std::unique_ptr<IoBackend> backend, Config cfg, std::string config_path);

        ~Acquisition();

        void set_publisher(Publisher p);

        void start();

        void stop();

        // Thread-sichere Schnittstelle für den Web-Server
        nlohmann::json snapshot() const;

        nlohmann::json measurements_json() const;

        std::string measurements_csv(const std::string &unit) const;

        size_t import_measurements(const std::vector<Measurement> &ms);

        Config config() const;

        ConfigUpdateResult update_config(const Config &next);

        void reset_statistics();

        bool delete_measurement(uint64_t id);

        nlohmann::json info() const;

        // Im Acquisition-Thread ausgeführt; Ergebnis innerhalb von timeout_ms
        CalResult tare();

        CalResult clear_tare();

        CalResult calibration(const nlohmann::json &request);

        CalResult set_sim_load(std::optional<double> grams);

        // Kommandos und Diagnose der Klemme (Gerätebeschreibung)
        CalResult device_command(const std::string &id);

        nlohmann::json device_diagnostics();

        // Debug (siehe README)
        nlohmann::json debug_pdos();

        CalResult debug_override(std::optional<uint8_t> sub, std::optional<bool> value);

        nlohmann::json debug_sdo(const nlohmann::json &req);

        nlohmann::json debug_init_sequence() const;

        void reinit();

    private:
        /// Kommando für den Acquisition-Thread
        struct Request {
            std::function<CalResult()> fn; ///< auszuführende Funktion
            std::promise<CalResult> result; ///< Ergebnis für den Aufrufer
        };

        void run();

        /// Wartezeit auf Kommandos im Acquisition-Thread [ms]
        static constexpr int kRequestTimeoutMs = 3000;

        CalResult submit(std::function<CalResult()> fn, int timeout_ms = kRequestTimeoutMs);

        nlohmann::json submit_json(std::function<nlohmann::json()> fn, int timeout_ms = kRequestTimeoutMs);

        void drain_requests(bool backend_ok);

        void apply_new_config_locked(const Config &next);

        void publish(const nlohmann::json &j);

        nlohmann::json sample_json_locked() const;

        nlohmann::json display_value_locked() const;

        nlohmann::json measurement_json(const Measurement &m) const;

        CalResult handle_calibration(const nlohmann::json &req, double t);

        std::unique_ptr<IoBackend> backend_; ///< Prozessdatenquelle
        std::string config_path_; ///< Pfad der Konfigurationsdatei
        std::thread thread_; ///< Acquisition-Thread
        std::atomic<bool> running_{false}; ///< Thread läuft
        std::atomic<bool> reinit_{false}; ///< Neuinitialisierung angefordert

        std::mutex req_mtx_; ///< schützt requests_
        std::deque<std::shared_ptr<Request> > requests_; ///< wartende Kommandos

        mutable std::mutex mtx_; ///< schützt alle folgenden Member
        Config cfg_; ///< aktive Konfiguration
        Config cfg_saved_; ///< gespeichert (inkl. Änderungen, die einen Neustart erfordern)
        bool restart_required_ = false; ///< gespeicherte Änderungen erfordern Neustart
        bool cfg_pending_ = false; ///< neue Konfiguration im nächsten Zyklus übernehmen
        bool device_pending_ = false; ///< Klemmenparameter im nächsten Zyklus schreiben
        Characteristic char_; ///< Kennlinie
        std::optional<double> tare_g_; ///< Tara [g], std::nullopt = keine
        std::deque<double> gross_hist_; ///< gültige Bruttowerte für die Tara-Mittelung
        std::deque<double> display_hist_; ///< gültige Bruttowerte für den Anzeigewert (ui.display_average)
        MeasurementEngine engine_; ///< Erkennung gültiger Messungen
        TemporaryStatistics stats_; ///< Statistik
        std::deque<Measurement> history_; ///< gehaltene Messungen
        Sample last_; ///< letztes Sample
        CalibrationController cal_; ///< nur im Acquisition-Thread verändert
        DeviceCommandRunner cmd_; ///< Kommandos der Klemme, nur im Acquisition-Thread
        std::map<uint8_t, bool> ctrl_override_; ///< Debug: übersteuerte Steuerbits, nur im Acquisition-Thread
        nlohmann::json cal_json_; ///< letzter Kalibrierzustand für Snapshots
        std::string backend_state_ = "init"; ///< init | op | error
        std::string backend_error_; ///< Fehlertext bei error
        uint64_t cycles_ = 0; ///< ausgeführte Zyklen
        uint64_t overruns_ = 0; ///< Zyklen mit Überlauf
        double max_cycle_ms_ = 0.0; ///< längste Zyklusdauer
        double now_ = 0.0; ///< Zeit des aktuellen Zyklus [s]

        std::mutex pub_mtx_; ///< schützt publisher_
        Publisher publisher_; ///< Empfänger der Ereignisse
    };
} // namespace ew
