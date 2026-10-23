#include "statistics.hpp"

#include <algorithm>
#include <cmath>

namespace ew {

void to_json(nlohmann::json& j, const StatSummary& s) {
    j = {{"n", s.n},       {"mean", s.mean}, {"stddev", s.stddev}, {"min", s.min},
         {"max", s.max},   {"range", s.range}, {"last", s.last}};
}

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

void RunningStats::reset() { *this = RunningStats{}; }

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

void RollingStats::add(double x) {
    buf_.push_back(x);
    while (buf_.size() > size_) buf_.pop_front();
}

void RollingStats::resize(uint32_t size) {
    size_ = std::max<uint32_t>(1, size);
    while (buf_.size() > size_) buf_.pop_front();
}

StatSummary RollingStats::summary() const {
    RunningStats r;
    for (double x : buf_) r.add(x);
    return r.summary();
}

nlohmann::json TemporaryStatistics::to_json() const {
    return {{"session", session_.summary()}, {"rolling", rolling_.summary()}};
}

} // namespace ew
