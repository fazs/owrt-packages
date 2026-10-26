#pragma once

#include <deque>
#include <vector>

#include <nlohmann/json.hpp>

namespace ew {
    /// Statistics of a series of values.
    struct StatSummary {
        uint64_t n = 0;      ///< count
        double mean = 0.0;   ///< mean
        double stddev = 0.0; ///< sample standard deviation (n-1)
        double min = 0.0;    ///< minimum
        double max = 0.0;    ///< maximum
        double range = 0.0;  ///< range max - min
        double last = 0.0;   ///< most recently added value
    };

    void to_json(nlohmann::json &j, const StatSummary &s);

    /// Online statistics (Welford), O(1) per value.
    class RunningStats {
    public:
        void add(double x);

        void remove(double x, const std::vector<double> &held);

        void reset();

        [[nodiscard]] StatSummary summary () const;

    private:
        uint64_t n_ = 0;
        double mean_ = 0.0, m2_ = 0.0, min_ = 0.0, max_ = 0.0, last_ = 0.0;
    };

    /// Statistics over the last N values.
    class RollingStats {
    public:
        /// @param size window size N
        explicit RollingStats(uint32_t size) : size_(size) {
        }

        void add(double x);

        /// Clears the window.
        void reset() { buf_.clear(); }

        void resize(uint32_t size);

        void rebuild(const std::vector<double> &held);

        [[nodiscard]] StatSummary summary () const;

    private:
        uint32_t size_;
        std::deque<double> buf_;
    };

    /// Temporary statistics (RAM only): session (since reset) + rolling window.
    class TemporaryStatistics {
    public:
        /// @param rolling size of the rolling window
        explicit TemporaryStatistics(uint32_t rolling) : rolling_(rolling) {
        }

        /// Adds a measurement to session and window.
        void add(double x) {
            session_.add(x);
            rolling_.add(x);
        }

        /// Resets session and window.
        void reset() {
            session_.reset();
            rolling_.reset();
        }

        void remove(double x, const std::vector<double> &held);

        /// Changes the size of the rolling window.
        void set_rolling_window(uint32_t n) { rolling_.resize(n); }

        [[nodiscard]] nlohmann::json to_json() const;

    private:
        RunningStats session_;  ///< since reset
        RollingStats rolling_;  ///< last N
    };
} // namespace ew
