#pragma once

#include <deque>
#include <vector>

#include <nlohmann/json.hpp>

namespace ew {
    /// Kennzahlen einer Wertereihe.
    struct StatSummary {
        uint64_t n = 0;      ///< Anzahl
        double mean = 0.0;   ///< Mittelwert
        double stddev = 0.0; ///< Stichproben-Standardabweichung (n-1)
        double min = 0.0;    ///< Minimum
        double max = 0.0;    ///< Maximum
        double range = 0.0;  ///< Spannweite max - min
        double last = 0.0;   ///< zuletzt hinzugefügter Wert
    };

    void to_json(nlohmann::json &j, const StatSummary &s);

    /// Online-Statistik (Welford), O(1) pro Wert.
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

    /// Statistik über die letzten N Werte.
    class RollingStats {
    public:
        /// @param size Fenstergröße N
        explicit RollingStats(uint32_t size) : size_(size) {
        }

        void add(double x);

        /// Leert das Fenster.
        void reset() { buf_.clear(); }

        void resize(uint32_t size);

        void rebuild(const std::vector<double> &held);

        [[nodiscard]] StatSummary summary () const;

    private:
        uint32_t size_;
        std::deque<double> buf_;
    };

    /// Temporäre Statistik (nur RAM): Session (seit Reset) + gleitendes Fenster.
    class TemporaryStatistics {
    public:
        /// @param rolling Größe des gleitenden Fensters
        explicit TemporaryStatistics(uint32_t rolling) : rolling_(rolling) {
        }

        /// Fügt eine Messung zu Session und Fenster hinzu.
        void add(double x) {
            session_.add(x);
            rolling_.add(x);
        }

        /// Setzt Session und Fenster zurück.
        void reset() {
            session_.reset();
            rolling_.reset();
        }

        void remove(double x, const std::vector<double> &held);

        /// Ändert die Größe des gleitenden Fensters.
        void set_rolling_window(uint32_t n) { rolling_.resize(n); }

        [[nodiscard]] nlohmann::json to_json() const;

    private:
        RunningStats session_;  ///< seit Reset
        RollingStats rolling_;  ///< letzte N
    };
} // namespace ew
