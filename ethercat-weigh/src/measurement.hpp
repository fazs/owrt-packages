#pragma once

#include <deque>
#include <optional>
#include <string>

#include "config.hpp"
#include "sample.hpp"

namespace ew {
    /// Zustand der MeasurementEngine.
    enum class MeasState {
        Fault, ///< Sample ungültig (Link, WKC, Error, Overrange, Data invalid, Calibration)
        Idle, ///< Last < min_load
        Settling, ///< Last >= min_load, freigegeben, Stabilitätsfenster wird gefüllt
        WaitUnload ///< Messung übernommen; Freigabe erst bei Last < zero_threshold
    };

    const char *to_string(MeasState s);

    /// Kennzahlen des aktuellen Stabilitätsfensters (Massen in Gramm).
    struct WindowStats {
        uint32_t n = 0; ///< Samples im Fenster
        double span_ms = 0.0; ///< Zeitspanne erstes bis letztes Sample
        double mean = 0.0; ///< Mittelwert
        double min = 0.0; ///< Minimum
        double max = 0.0; ///< Maximum
        double range = 0.0; ///< Spannweite max - min
        double stddev = 0.0; ///< Standardabweichung (n-1)
    };

    /// Eine übernommene Messung (Massen in Gramm).
    struct Measurement {
        uint64_t id = 0; ///< fortlaufende Nummer
        double t = 0.0; ///< Zeitpunkt der Übernahme [s]
        double value = 0.0; ///< Mittelwert des Stabilitätsfensters
        double stddev = 0.0; ///< Standardabweichung im Fenster
        double range = 0.0; ///< Spannweite im Fenster
        uint32_t n = 0; ///< Samples im Fenster
        double settle_ms = 0.0; ///< Zeit von Belastung bis Übernahme
    };

    /**
     * @brief Zustandsautomat "gültige Messung" für statische Wägung.
     *
     * Übernahmebedingung (alle im Fenster der Länge stable_time_ms):
     * max - min <= tolerance UND stddev <= sigma_max
     * UND (optional) Steady-state-Bit 0x6000:09 gesetzt.
     *
     * @see MeasState
     */
    class MeasurementEngine {
    public:
        explicit MeasurementEngine(const MeasurementConfig &p = {});

        void set_params(const MeasurementConfig &p);

        /// aktuelle Parameter
        const MeasurementConfig &params() const { return p_; }

        std::optional<Measurement> process(const Sample &s);

        /// aktueller Zustand
        [[nodiscard]] MeasState state() const { return state_; }
        /// Fehlergrund im Zustand Fault (siehe sample_valid())
        [[nodiscard]] const std::string &fault_reason() const { return fault_reason_; }
        /// Kennzahlen des aktuellen Fensters
        [[nodiscard]] const WindowStats &window() const { return win_; }
        /// true, wenn eine neue Messung übernommen werden darf
        bool armed() const { return armed_; }

        void reset();

    private:
        /// Ein Wert im Fenster
        struct Pt {
            double t; ///< Zeit [s]
            double v; ///< Nettowert [g]
        };

        void compute_window();

        MeasurementConfig p_; ///< Parameter
        MeasState state_ = MeasState::Idle; ///< Zustand
        std::string fault_reason_; ///< Fehlergrund bei Fault
        bool armed_ = true; ///< Übernahme freigegeben
        double load_since_ = -1.0; ///< Beginn der Belastung [s], -1 = unbelastet
        std::deque<Pt> buf_; ///< Stabilitätsfenster
        WindowStats win_; ///< Kennzahlen von buf_
        uint64_t next_id_ = 1; ///< Nummer der nächsten Messung
    };
} // namespace ew
