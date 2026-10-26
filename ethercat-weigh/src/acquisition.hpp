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
    /// Result of Acquisition::update_config().
    struct ConfigUpdateResult {
        bool ok = false; ///< saved and applied
        bool restart_required = false; ///< ethercat/http changed, takes effect only after restart
        std::vector<std::string> errors; ///< validation or save errors
    };

    /**
     * @brief Cyclic acquisition thread.
     *
     * Process data -> characteristic -> tare -> MeasurementEngine -> TemporaryStatistics.
     * Events are distributed as JSON via the publisher. Commands from the web server
     * (tare, calibration, debug) are executed via a queue in the acquisition thread,
     * because SOEM is not thread-safe.
     */
    class Acquisition {
    public:
        /// Receiver of the JSON events (WebSocket broadcast)
        using Publisher = std::function<void(const std::string &)>;

        Acquisition(std::unique_ptr<IoBackend> backend, Config cfg, std::string config_path);

        ~Acquisition();

        void set_publisher(Publisher p);

        void start();

        void stop();

        // Thread-safe interface for the web server
        nlohmann::json snapshot() const;

        nlohmann::json measurements_json() const;

        std::string measurements_csv(const std::string &unit) const;

        size_t import_measurements(const std::vector<Measurement> &ms);

        Config config() const;

        ConfigUpdateResult update_config(const Config &next);

        void reset_statistics();

        bool delete_measurement(uint64_t id);

        nlohmann::json info() const;

        // Executed in the acquisition thread; result within timeout_ms
        CalResult tare();

        CalResult clear_tare();

        CalResult calibration(const nlohmann::json &request);

        CalResult set_sim_load(std::optional<double> grams);

        // Commands and diagnostics of the terminal (device description)
        CalResult device_command(const std::string &id);

        nlohmann::json device_diagnostics();

        // Debug (see README)
        nlohmann::json debug_pdos();

        CalResult debug_override(std::optional<uint8_t> sub, std::optional<bool> value);

        nlohmann::json debug_sdo(const nlohmann::json &req);

        nlohmann::json debug_init_sequence() const;

        void reinit();

    private:
        /// Command for the acquisition thread
        struct Request {
            std::function<CalResult()> fn; ///< function to execute
            std::promise<CalResult> result; ///< result for the caller
        };

        void run();

        /// Waiting time for commands in the acquisition thread [ms]
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

        std::unique_ptr<IoBackend> backend_; ///< process data source
        std::string config_path_; ///< path of the configuration file
        std::thread thread_; ///< acquisition thread
        std::atomic<bool> running_{false}; ///< thread is running
        std::atomic<bool> reinit_{false}; ///< reinitialization requested

        std::mutex req_mtx_; ///< protects requests_
        std::deque<std::shared_ptr<Request> > requests_; ///< pending commands

        mutable std::mutex mtx_; ///< protects all following members
        Config cfg_; ///< active configuration
        Config cfg_saved_; ///< saved (incl. changes that require a restart)
        bool restart_required_ = false; ///< saved changes require a restart
        bool cfg_pending_ = false; ///< take over the new configuration in the next cycle
        bool device_pending_ = false; ///< write terminal parameters in the next cycle
        Characteristic char_; ///< characteristic
        std::optional<double> tare_g_; ///< tare [g], std::nullopt = none
        std::deque<double> gross_hist_; ///< valid gross values for tare averaging
        std::deque<double> display_hist_; ///< valid gross values for the display value (ui.display_average)
        MeasurementEngine engine_; ///< detection of valid measurements
        TemporaryStatistics stats_; ///< statistics
        std::deque<Measurement> history_; ///< held measurements
        Sample last_; ///< last sample
        CalibrationController cal_; ///< modified only in the acquisition thread
        DeviceCommandRunner cmd_; ///< commands of the terminal, only in the acquisition thread
        std::map<uint8_t, bool> ctrl_override_; ///< debug: overridden control bits, only in the acquisition thread
        nlohmann::json cal_json_; ///< last calibration state for snapshots
        std::string backend_state_ = "init"; ///< init | op | error
        std::string backend_error_; ///< error text in error
        uint64_t cycles_ = 0; ///< executed cycles
        uint64_t overruns_ = 0; ///< cycles with overrun
        double max_cycle_ms_ = 0.0; ///< longest cycle duration
        double now_ = 0.0; ///< time of the current cycle [s]

        std::mutex pub_mtx_; ///< protects publisher_
        Publisher publisher_; ///< receiver of the events
    };
} // namespace ew
