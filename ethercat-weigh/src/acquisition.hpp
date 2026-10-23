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
#include "io_backend.hpp"
#include "measurement.hpp"
#include "statistics.hpp"

namespace ew {

struct ConfigUpdateResult {
    bool ok = false;
    bool restart_required = false;
    std::vector<std::string> errors;
};

// Zyklischer Acquisition-Thread: Prozessdaten -> Kennlinie -> Tara -> MeasurementEngine
// -> TemporaryStatistics. Ereignisse werden als JSON über den Publisher verteilt.
// Kommandos aus dem Web-Server (Tara, Kalibrierung) werden über eine Queue im
// Acquisition-Thread ausgeführt, da SOEM nicht threadsicher ist.
class Acquisition {
public:
    using Publisher = std::function<void(const std::string&)>;

    Acquisition(std::unique_ptr<IoBackend> backend, Config cfg, std::string config_path);
    ~Acquisition();

    void set_publisher(Publisher p);
    void start();
    void stop();

    // Thread-sichere Schnittstelle für den Web-Server
    nlohmann::json snapshot() const;
    nlohmann::json measurements_json() const;
    std::string measurements_csv(const std::string& unit) const;
    Config config() const;
    ConfigUpdateResult update_config(const Config& next);
    void reset_statistics();
    nlohmann::json info() const;

    // Im Acquisition-Thread ausgeführt; Ergebnis innerhalb von timeout_ms
    CalResult tare();
    CalResult clear_tare();
    CalResult calibration(const nlohmann::json& request);
    CalResult set_sim_load(std::optional<double> grams);

    // Debug (siehe README): PDO-Katalog mit aktuellen Werten, Übersteuerung der Steuerbits
    // 0x7000:sub (nullopt hebt auf), einzelner SDO-Zugriff, Initialisierungssequenz
    nlohmann::json debug_pdos();
    CalResult debug_override(std::optional<uint8_t> sub, std::optional<bool> value);
    nlohmann::json debug_sdo(const nlohmann::json& request);
    nlohmann::json debug_init_sequence() const;
    // Prozessdatenquelle schließen und neu öffnen (INIT -> OP inkl. Initialisierungssequenz)
    void reinit();

private:
    struct Request {
        std::function<CalResult()> fn;
        std::promise<CalResult> result;
    };

    void run();
    CalResult submit(std::function<CalResult()> fn, int timeout_ms = 3000);
    nlohmann::json submit_json(std::function<nlohmann::json()> fn, int timeout_ms = 3000);
    void drain_requests(bool backend_ok);
    void apply_new_config_locked(const Config& next);
    void publish(const nlohmann::json& j);
    nlohmann::json sample_json_locked() const;
    nlohmann::json measurement_json(const Measurement& m) const;
    CalResult handle_calibration(const nlohmann::json& req, double t);

    std::unique_ptr<IoBackend> backend_;
    std::string config_path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reinit_{false};

    std::mutex req_mtx_;
    std::deque<std::shared_ptr<Request>> requests_;

    mutable std::mutex mtx_;           // schützt alles darunter
    Config cfg_;        // aktiv
    Config cfg_saved_;  // gespeichert (inkl. Änderungen, die einen Neustart erfordern)
    bool restart_required_ = false;
    bool cfg_pending_ = false;
    bool device_pending_ = false;
    Characteristic char_;
    std::optional<double> tare_g_;
    std::deque<double> gross_hist_;    // gültige Bruttowerte für die Tara-Mittelung
    MeasurementEngine engine_;
    TemporaryStatistics stats_;
    std::deque<Measurement> history_;
    Sample last_;
    CalibrationController cal_;        // nur im Acquisition-Thread verändert
    std::map<uint8_t, bool> ctrl_override_;  // Debug, nur im Acquisition-Thread
    nlohmann::json cal_json_;
    std::string backend_state_ = "init";  // init | op | error
    std::string backend_error_;
    uint64_t cycles_ = 0;
    uint64_t overruns_ = 0;
    double max_cycle_ms_ = 0.0;
    double now_ = 0.0;

    std::mutex pub_mtx_;
    Publisher publisher_;
};

} // namespace ew
