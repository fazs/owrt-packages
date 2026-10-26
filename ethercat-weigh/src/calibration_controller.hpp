#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "io_backend.hpp"
#include "sample.hpp"

namespace ew {
    /// Result of a command (calibration, tare, debug).
    struct CalResult {
        bool ok = false; ///< command executed
        std::string error; ///< error text if ok == false
    };

    /**
     * @brief Sequence control of the calibration.
     *
     * Used exclusively in the acquisition thread.
     *
     * Application ("application"):
     * start -> capture(mass) per point (zero point = mass 0) -> commit (>= 2 points).
     * Raw value per point = mean of Value (Real) over capture_time_ms.
     *
     * Terminal ("device", Beckhoff procedure "calibration in the plant"):
     * - start: scale factor 1, gain 1, tare 0, filter IIR8, nominal load, gravity
     * - zero: command 0x0101 after >= stable_time_s of standstill
     * - ref: write reference load (>= 20 % of nominal load), command 0x0102
     * - afterwards: command 0x0000, restore filter and scale factor from the config,
     *   read rated output (0x8000:23) and zero balance (0x8000:25)
     * - commit: take the result over into the config
     */
    class CalibrationController {
    public:
        /// Calibration in the terminal: reference load at least this fraction of the nominal load (Beckhoff: 20 %)
        static constexpr double kMinReferenceRatio = 0.2;

        /// true while a calibration is running
        [[nodiscard]] bool active() const { return kind_ != Kind::None; }

        CalResult start_application(const Config &cfg);

        CalResult capture(double mass_g, double t);

        CalResult remove_point(size_t index);

        CalResult start_device(const Config &cfg, double nominal_load, const std::string &unit,
                               IoBackend &io);

        CalResult device_zero(IoBackend &io, double t);

        CalResult device_reference(double mass_g, IoBackend &io, double t);

        CalResult commit(const Config &current, Config &out);

        void cancel(const Config &cfg, IoBackend &io);

        void step(const Sample &s, IoBackend &io, const Config &cfg);

        [[nodiscard]] nlohmann::json to_json() const;

        /// Captured calibration point (calibration in the application).
        struct AppPoint {
            double raw = 0.0; ///< mean of Value (Real)
            double mass_g = 0.0; ///< assigned mass [g]
            double stddev = 0.0; ///< standard deviation of the raw value
            uint32_t n = 0; ///< averaged samples
        };

        /// captured calibration points, sorted by mass
        [[nodiscard]] const std::vector<AppPoint> &points() const { return points_; }
        /// application: idle|capturing; device: prepared|zero_busy|zero_done|ref_busy|done
        [[nodiscard]] const std::string &phase() const { return phase_; }

    private:
        /// Kind of the running calibration
        enum class Kind { None, Application, Device };

        void reset();

        void finish_device(IoBackend &io, const Config &cfg);

        Kind kind_ = Kind::None; ///< running calibration
        std::string phase_; ///< see phase()
        std::string error_; ///< last error
        std::string message_; ///< hint for the operator

        // Application
        std::vector<AppPoint> points_; ///< captured calibration points
        bool capturing_ = false; ///< capture in progress
        double cap_mass_g_ = 0.0; ///< mass of the current point [g]
        double cap_start_ = 0.0; ///< start of the capture [s]
        double cap_duration_ = 0.0; ///< capture duration [s]
        uint32_t cap_n_ = 0; ///< samples averaged so far
        double cap_mean_ = 0.0; ///< running mean (Welford)
        double cap_m2_ = 0.0; ///< running sum of squares (Welford)

        // Terminal
        std::string device_unit_; ///< unit of nominal/reference load
        double nominal_ = 0.0; ///< nominal load in device_unit
        double reference_ = 0.0; ///< reference load in device_unit
        double band_ = 0.0; ///< standstill band in device_unit
        double stable_time_s_ = 0.0; ///< required standstill [s]
        double stable_ref_ = 0.0; ///< reference value of the standstill detection
        double stable_since_ = -1.0; ///< start of standstill [s], -1 = no standstill
        double last_raw_ = 0.0; ///< last raw value
        double last_t_ = 0.0; ///< time of the last sample [s]
        double cmd_start_ = 0.0; ///< start of the running command [s]
        double next_poll_ = 0.0; ///< next poll of 0xFB00:02 [s]
        std::optional<double> rated_result_; ///< rated output 0x8000:23 read back
        std::optional<double> zero_result_; ///< zero balance 0x8000:25 read back
    };
} // namespace ew
