#include "statistics.hpp"

#include <algorithm>
#include <cmath>

namespace ew {
    /**
     * @brief JSON representation for nlohmann::json.
     * @param[out] j target
     * @param s statistics
     */
    void to_json(nlohmann::json &j, const StatSummary &s) {
        j = {
            {"n", s.n}, {"mean", s.mean}, {"stddev", s.stddev}, {"min", s.min},
            {"max", s.max}, {"range", s.range}, {"last", s.last}
        };
    }

    /**
     * @brief Adds a value (Welford).
     * @param x value
     */
    void RunningStats::add(double x) {
        ++n_;
        if (n_ == 1) {
            min_ = max_ = x;
        } else {
            min_ = std::min(min_, x);
            max_ = std::max(max_, x);
        }
        const double d = x - mean_;
        mean_ += d / static_cast<double>(n_);
        m2_ += d * (x - mean_);
        last_ = x;
    }

    /**
     * @brief Removes a value (Welford backwards).
     *
     * Count, mean and standard deviation are exact. Minimum, maximum and
     * last value are determined from @p held.
     *
     * @param x value to remove
     * @param held remaining held values in chronological order
     */
    void RunningStats::remove(double x, const std::vector<double> &held) {
        if (n_ <= 1 || held.empty()) {
            reset();
            for (double v: held) add(v);
            return;
        }
        const double mean_new = mean_;
        mean_ = (static_cast<double>(n_) * mean_new - x) / static_cast<double>(n_ - 1);
        m2_ = std::max(0.0, m2_ - (x - mean_) * (x - mean_new));
        --n_;
        const auto [mn, mx] = std::minmax_element(held.begin(), held.end());
        min_ = *mn;
        max_ = *mx;
        last_ = held.back();
    }

    /// @brief Resets all statistics.
    void RunningStats::reset() { *this = RunningStats{}; }

    /**
     * @brief Current statistics.
     * @return statistics; all values 0 for n = 0
     */
    StatSummary RunningStats::summary() const {
        StatSummary s;
        s.n = n_;
        if (n_ == 0) return s;
        s.mean = mean_;
        s.stddev = n_ > 1 ? std::sqrt(m2_ / static_cast<double>(n_ - 1)) : 0.0;
        s.min = min_;
        s.max = max_;
        s.range = max_ - min_;
        s.last = last_;
        return s;
    }

    /**
     * @brief Adds a value; the oldest one drops out when the window is full.
     * @param x value
     */
    void RollingStats::add(double x) {
        buf_.push_back(x);
        while (buf_.size() > size_) buf_.pop_front();
    }

    /**
     * @brief Changes the window size; surplus oldest values are discarded.
     * @param size new window size (at least 1)
     */
    void RollingStats::resize(uint32_t size) {
        size_ = std::max<uint32_t>(1, size);
        while (buf_.size() > size_) buf_.pop_front();
    }

    /**
     * @brief Rebuilds the window from the last N held values.
     * @param held held values in chronological order
     */
    void RollingStats::rebuild(const std::vector<double> &held) {
        buf_.assign(held.size() > size_ ? held.end() - size_ : held.begin(), held.end());
    }

    /**
     * @brief Statistics over the window.
     * @return statistics of the last N values
     */
    StatSummary RollingStats::summary() const {
        RunningStats r;
        for (double x: buf_) r.add(x);
        return r.summary();
    }

    /**
     * @brief Removes a single value from session and window.
     *
     * If all values since the reset are still held, the session is recomputed exactly;
     * otherwise minimum/maximum are only determined over the held values.
     *
     * @param x value to remove
     * @param held remaining held values in chronological order
     */
    void TemporaryStatistics::remove(double x, const std::vector<double> &held) {
        if (session_.summary().n == held.size() + 1) {
            session_.reset();
            for (double v: held) session_.add(v);
        } else {
            session_.remove(x, held);
        }
        rolling_.rebuild(held);
    }

    /**
     * @brief JSON with session and window statistics.
     * @return {"session": ..., "rolling": ...}
     */
    nlohmann::json TemporaryStatistics::to_json() const {
        return {{"session", session_.summary()}, {"rolling", rolling_.summary()}};
    }
} // namespace ew
