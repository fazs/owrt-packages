#include "measurement.hpp"
#include "units.hpp"

#include <algorithm>
#include <cmath>

namespace ew {

namespace {
/// Mindestanzahl Samples im Fenster (Spannweite und Standardabweichung brauchen zwei Werte)
constexpr size_t kMinWindowSamples = 2U;
} // namespace

/**
 * @brief Name eines Zustands für JSON und Anzeige.
 * @param s Zustand
 * @return "fault", "idle", "settling" oder "wait_unload"
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

/// @param p Parameter (Massen in Gramm)
MeasurementEngine::MeasurementEngine(const MeasurementConfig& p) : p_(p) {}

/**
 * @brief Übernimmt neue Parameter und leert das Fenster.
 * @param p Parameter (Massen in Gramm)
 */
void MeasurementEngine::set_params(const MeasurementConfig& p) {
    p_ = p;
    buf_.clear();
    win_ = {};
}

/// @brief Zurück in Idle: Fenster leeren, Freigabe setzen (z. B. nach Tara).
void MeasurementEngine::reset() {
    state_ = MeasState::Idle;
    fault_reason_.clear();
    armed_ = true;
    load_since_ = -1.0;
    buf_.clear();
    win_ = {};
}

/**
 * @brief Vergibt die nächste Nummer, z. B. für importierte Messungen.
 * @return Nummer; folgende Messungen zählen danach weiter
 */
uint64_t MeasurementEngine::get_next_id() { return next_id_++; }

/// @brief Berechnet win_ aus buf_ (Welford über das Fenster).
void MeasurementEngine::compute_window() {
    win_ = {};
    if (buf_.empty()) return;
    win_.n = static_cast<uint32_t>(buf_.size());
    win_.span_ms = (buf_.back().t - buf_.front().t) * kMsPerS;
    double mn = buf_.front().v, mx = mn, mean = 0.0, m2 = 0.0;
    uint32_t k = 0;
    for (const auto& p : buf_) {  // Welford über das Fenster
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
 * @brief Verarbeitet ein Sample (einmal je Zyklus).
 * @param s Sample mit Nettowert value [g]
 * @return Messung genau im Übernahmezyklus, sonst std::nullopt
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
    // Kleinstes Fenster behalten, dessen Spannweite >= stable_time ist
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
