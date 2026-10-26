#pragma once

#include <deque>
#include <optional>
#include <string>

#include "config.hpp"
#include "sample.hpp"

namespace ew {
    /// State of the MeasurementEngine.
    enum class MeasState {
        Fault, ///< sample invalid (link, WKC, error, overrange, data invalid, calibration)
        Idle, ///< load < min_load
        Settling, ///< load >= min_load, armed, stability window is being filled
        WaitUnload ///< measurement captured; re-armed only when load < zero_threshold
    };

    const char *to_string(MeasState s);

    /// Statistics of the current stability window (masses in grams).
    struct WindowStats {
        uint32_t n = 0; ///< samples in the window
        double span_ms = 0.0; ///< time span from first to last sample
        double mean = 0.0; ///< mean
        double min = 0.0; ///< minimum
        double max = 0.0; ///< maximum
        double range = 0.0; ///< range max - min
        double stddev = 0.0; ///< standard deviation (n-1)
    };

    /// A captured measurement (masses in grams).
    struct Measurement {
        uint64_t id = 0; ///< sequential number
        double t = 0.0; ///< time of capture [s]
        double value = 0.0; ///< mean of the stability window
        double stddev = 0.0; ///< standard deviation in the window
        double range = 0.0; ///< range in the window
        uint32_t n = 0; ///< samples in the window
        double settle_ms = 0.0; ///< time from loading to capture
    };

    /**
     * @brief State machine "valid measurement" for static weighing.
     *
     * Capture condition (all within the window of length stable_time_ms):
     * max - min <= tolerance AND stddev <= sigma_max
     * AND (optionally) steady state bit 0x6000:09 set.
     *
     * @see MeasState
     */
    class MeasurementEngine {
    public:
        explicit MeasurementEngine(const MeasurementConfig &p = {});

        void set_params(const MeasurementConfig &p);

        /// current parameters
        const MeasurementConfig &params() const { return p_; }

        std::optional<Measurement> process(const Sample &s);

        /// current state
        [[nodiscard]] MeasState state() const { return state_; }
        /// reason for failure in state Fault (see sample_valid())
        [[nodiscard]] const std::string &fault_reason() const { return fault_reason_; }
        /// statistics of the current window
        [[nodiscard]] const WindowStats &window() const { return win_; }
        /// true if a new measurement may be captured
        bool armed() const { return armed_; }

        void reset();

        uint64_t get_next_id();

    private:
        /// One value in the window
        struct Pt {
            double t; ///< time [s]
            double v; ///< net value [g]
        };

        void compute_window();

        MeasurementConfig p_; ///< parameters
        MeasState state_ = MeasState::Idle; ///< state
        std::string fault_reason_; ///< reason for failure in Fault
        bool armed_ = true; ///< capture armed
        double load_since_ = -1.0; ///< start of loading [s], -1 = unloaded
        std::deque<Pt> buf_; ///< stability window
        WindowStats win_; ///< statistics of buf_
        uint64_t next_id_ = 1; ///< number of the next measurement
    };
} // namespace ew
