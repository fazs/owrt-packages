#include "measurement.hpp"
#include "units.hpp"

#include <algorithm>
#include <cmath>

namespace ew {

namespace {
/// Minimum number of samples in the window (range and standard deviation need two values)
constexpr size_t kMinWindowSamples = 2U;
} // namespace

/**
 * @brief Name of a state for JSON and display.
 * @param s state
 * @return "fault", "idle", "settling" or "wait_unload"
 */
const char* to_string(MeasState s) {
    switch (s) {
    case MeasState::Fault: return "fault";
    case MeasState::Idle: return "idle";
    case MeasState::Settling: return "settling";
    case MeasState::WaitUnload: return "wait_unload";
    }
    return "unknown";
}

/// @param p parameters (masses in grams)
MeasurementEngine::MeasurementEngine(const MeasurementConfig& p) : p_(p) {}

/**
 * @brief Takes over new parameters and clears the window.
 * @param p parameters (masses in grams)
 */
void MeasurementEngine::set_params(const MeasurementConfig& p) {
    p_ = p;
    buf_.clear();
    win_ = {};
}

/// @brief Back to Idle: clear the window, re-arm (e.g. after tare).
void MeasurementEngine::reset() {
    state_ = MeasState::Idle;
    fault_reason_.clear();
    armed_ = true;
    load_since_ = -1.0;
    buf_.clear();
    win_ = {};
}

/**
 * @brief Assigns the next number, e.g. for imported measurements.
 * @return number; following measurements continue counting from it
 */
uint64_t MeasurementEngine::get_next_id() { return next_id_++; }

/// @brief Computes win_ from buf_ (Welford over the window).
void MeasurementEngine::compute_window() {
    win_ = {};
    if (buf_.empty()) return;
    win_.n = static_cast<uint32_t>(buf_.size());
    win_.span_ms = (buf_.back().t - buf_.front().t) * kMsPerS;
    double mn = buf_.front().v, mx = mn, mean = 0.0, m2 = 0.0;
    uint32_t k = 0;
    for (const auto& p : buf_) {  // Welford over the window
        ++k;
        const double d = p.v - mean;
        mean += d / k;
        m2 += d * (p.v - mean);
        mn = std::min(mn, p.v);
        mx = std::max(mx, p.v);
    }
    win_.mean = mean;
    win_.min = mn;
    win_.max = mx;
    win_.range = mx - mn;
    win_.stddev = k > 1 ? std::sqrt(m2 / (k - 1)) : 0.0;
}

/**
 * @brief Processes a sample (once per cycle).
 * @param s sample with net value value [g]
 * @return measurement exactly in the capture cycle, otherwise std::nullopt
 */
std::optional<Measurement> MeasurementEngine::process(const Sample& s) {
    std::string reason;
    if (!sample_valid(s, &reason)) {
        state_ = MeasState::Fault;
        fault_reason_ = reason;
        buf_.clear();
        win_ = {};
        return std::nullopt;
    }
    fault_reason_.clear();
    const double v = s.value;

    if (v < p_.zero_threshold) armed_ = true;

    if (v < p_.min_load) {
        state_ = MeasState::Idle;
        load_since_ = -1.0;
        buf_.clear();
        win_ = {};
        return std::nullopt;
    }
    if (load_since_ < 0.0) load_since_ = s.t;

    if (!armed_) {
        state_ = MeasState::WaitUnload;
        buf_.clear();
        win_ = {};
        return std::nullopt;
    }

    state_ = MeasState::Settling;
    buf_.push_back({s.t, v});
    // Keep the smallest window whose span is >= stable_time
    const double tw = p_.stable_time_ms / kMsPerS;
    while (buf_.size() > kMinWindowSamples && (buf_.back().t - buf_[1].t) >= tw) buf_.pop_front();
    compute_window();

    const bool long_enough = (win_.span_ms / kMsPerS) >= tw;
    const bool stable = win_.range <= p_.tolerance && win_.stddev <= p_.sigma_max;
    const bool dev_ok = !p_.use_device_steady_state || s.status.steady_state;
    if (!(long_enough && stable && dev_ok)) return std::nullopt;

    Measurement m;
    m.id = next_id_++;
    m.t = s.t;
    m.value = win_.mean;
    m.stddev = win_.stddev;
    m.range = win_.range;
    m.n = win_.n;
    m.settle_ms = (s.t - load_since_) * kMsPerS;

    armed_ = false;
    state_ = MeasState::WaitUnload;
    buf_.clear();
    return m;
}

} // namespace ew
