#pragma once

#include <cstdint>
#include <deque>

#include <nlohmann/json.hpp>

namespace ew {

struct StatSummary {
    uint64_t n = 0;
    double mean = 0.0;
    double stddev = 0.0;  // Stichproben-Standardabweichung (n-1)
    double min = 0.0;
    double max = 0.0;
    double range = 0.0;
    double last = 0.0;
};
void to_json(nlohmann::json& j, const StatSummary& s);

// Online-Statistik (Welford), O(1) pro Wert.
class RunningStats {
public:
    void add(double x);
    void reset();
    StatSummary summary() const;

private:
    uint64_t n_ = 0;
    double mean_ = 0.0, m2_ = 0.0, min_ = 0.0, max_ = 0.0, last_ = 0.0;
};

// Statistik über die letzten N Werte.
class RollingStats {
public:
    explicit RollingStats(uint32_t size = 50) : size_(size) {}
    void add(double x);
    void reset() { buf_.clear(); }
    void resize(uint32_t size);
    StatSummary summary() const;

private:
    uint32_t size_;
    std::deque<double> buf_;
};

// Temporäre Statistik: Session (seit Reset) + gleitendes Fenster.
class TemporaryStatistics {
public:
    explicit TemporaryStatistics(uint32_t rolling = 50) : rolling_(rolling) {}
    void add(double x) { session_.add(x); rolling_.add(x); }
    void reset() { session_.reset(); rolling_.reset(); }
    void set_rolling_window(uint32_t n) { rolling_.resize(n); }
    nlohmann::json to_json() const;

private:
    RunningStats session_;
    RollingStats rolling_;
};

} // namespace ew
