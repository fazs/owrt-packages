#include "statistics.hpp"

#include <algorithm>
#include <cmath>

namespace ew {
    /**
     * @brief JSON-Darstellung für nlohmann::json.
     * @param[out] j Ziel
     * @param s Kennzahlen
     */
    void to_json(nlohmann::json &j, const StatSummary &s) {
        j = {
            {"n", s.n}, {"mean", s.mean}, {"stddev", s.stddev}, {"min", s.min},
            {"max", s.max}, {"range", s.range}, {"last", s.last}
        };
    }

    /**
     * @brief Fügt einen Wert hinzu (Welford).
     * @param x Wert
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
     * @brief Entfernt einen Wert (Welford rückwärts).
     *
     * Anzahl, Mittelwert und Standardabweichung sind exakt. Minimum, Maximum und
     * letzter Wert werden aus @p held bestimmt.
     *
     * @param x zu entfernender Wert
     * @param held verbliebene gehaltene Werte in zeitlicher Reihenfolge
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

    /// @brief Setzt alle Kennzahlen zurück.
    void RunningStats::reset() { *this = RunningStats{}; }

    /**
     * @brief Aktuelle Kennzahlen.
     * @return Kennzahlen; bei n = 0 alle Werte 0
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
     * @brief Fügt einen Wert hinzu; der älteste fällt bei vollem Fenster heraus.
     * @param x Wert
     */
    void RollingStats::add(double x) {
        buf_.push_back(x);
        while (buf_.size() > size_) buf_.pop_front();
    }

    /**
     * @brief Ändert die Fenstergröße; überzählige älteste Werte werden verworfen.
     * @param size neue Fenstergröße (mindestens 1)
     */
    void RollingStats::resize(uint32_t size) {
        size_ = std::max<uint32_t>(1, size);
        while (buf_.size() > size_) buf_.pop_front();
    }

    /**
     * @brief Baut das Fenster aus den letzten N gehaltenen Werten neu auf.
     * @param held gehaltene Werte in zeitlicher Reihenfolge
     */
    void RollingStats::rebuild(const std::vector<double> &held) {
        buf_.assign(held.size() > size_ ? held.end() - size_ : held.begin(), held.end());
    }

    /**
     * @brief Kennzahlen über das Fenster.
     * @return Kennzahlen der letzten N Werte
     */
    StatSummary RollingStats::summary() const {
        RunningStats r;
        for (double x: buf_) r.add(x);
        return r.summary();
    }

    /**
     * @brief Entfernt einen einzelnen Wert aus Session und Fenster.
     *
     * Sind alle Werte seit Reset noch gehalten, wird die Session exakt neu berechnet;
     * sonst sind Minimum/Maximum nur über die gehaltenen Werte bestimmt.
     *
     * @param x zu entfernender Wert
     * @param held verbliebene gehaltene Werte in zeitlicher Reihenfolge
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
     * @brief JSON mit Session- und Fensterstatistik.
     * @return {"session": ..., "rolling": ...}
     */
    nlohmann::json TemporaryStatistics::to_json() const {
        return {{"session", session_.summary()}, {"rolling", rolling_.summary()}};
    }
} // namespace ew
