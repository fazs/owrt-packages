#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "config.hpp"
#include "sample.hpp"

namespace ew {

enum class MeasState { Fault, Idle, Settling, WaitUnload };
const char* to_string(MeasState s);

struct WindowStats {
    uint32_t n = 0;
    double span_ms = 0.0;
    double mean = 0.0;
    double min = 0.0;
    double max = 0.0;
    double range = 0.0;
    double stddev = 0.0;
};

struct Measurement {
    uint64_t id = 0;
    double t = 0.0;          // Zeitpunkt der Übernahme [s]
    double value = 0.0;      // Mittelwert des Stabilitätsfensters
    double stddev = 0.0;
    double range = 0.0;
    uint32_t n = 0;          // Samples im Fenster
    double settle_ms = 0.0;  // Zeit von Belastung bis Übernahme
};

// Zustandsautomat "gültige Messung" für statische Wägung.
//
//   Fault      : Sample ungültig (Link, WKC, Error, Overrange, Data invalid, Calibration)
//   Idle       : Last < min_load
//   Settling   : Last >= min_load, armed, Stabilitätsfenster wird gefüllt
//   WaitUnload : Messung übernommen; Re-Arm erst bei Last < zero_threshold
//
// Übernahmebedingung (alle im Fenster der Länge stable_time_ms):
//   max - min <= tolerance  UND  stddev <= sigma_max
//   UND (optional) Steady-state-Bit 0x6000:09 gesetzt
class MeasurementEngine {
public:
    explicit MeasurementEngine(const MeasurementConfig& p = {});

    void set_params(const MeasurementConfig& p);
    const MeasurementConfig& params() const { return p_; }

    // Verarbeitet ein Sample; liefert eine Messung genau im Übernahmezyklus.
    std::optional<Measurement> process(const Sample& s);

    MeasState state() const { return state_; }
    const std::string& fault_reason() const { return fault_reason_; }
    const WindowStats& window() const { return win_; }
    bool armed() const { return armed_; }
    void reset();

private:
    struct Pt { double t; double v; };
    void compute_window();

    MeasurementConfig p_;
    MeasState state_ = MeasState::Idle;
    std::string fault_reason_;
    bool armed_ = true;
    double load_since_ = -1.0;
    std::deque<Pt> buf_;
    WindowStats win_;
    uint64_t next_id_ = 1;
};

} // namespace ew
