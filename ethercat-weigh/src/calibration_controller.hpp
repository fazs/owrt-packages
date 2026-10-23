#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "io_backend.hpp"
#include "sample.hpp"

namespace ew {

struct CalResult {
    bool ok = false;
    std::string error;
};

// Ablaufsteuerung der Kalibrierung. Wird ausschließlich im Acquisition-Thread verwendet.
//
// Anwendung ("application"):
//   start -> capture(Masse) je Punkt (Nullpunkt = Masse 0) -> commit (>= 2 Punkte)
//   Rohwert je Punkt = Mittelwert von Value (Real) über capture_time_ms.
//
// Klemme ("device", Beckhoff-Verfahren "Abgleich in der Anlage"):
//   start  : Scale factor 1, Gain 1, Tara 0, Filter IIR8, Nominal load, Gravity
//   zero   : Kommando 0x0101 nach >= stable_time_s Stillstand
//   ref    : Reference load (>= 20 % Nennlast) schreiben, Kommando 0x0102
//   danach : Kommando 0x0000, Filter und Scale factor aus der Config zurück,
//            Rated output (0x8000:23) und Zero balance (0x8000:25) lesen
//   commit : Ergebnis in die Config übernehmen
class CalibrationController {
public:
    bool active() const { return kind_ != Kind::None; }

    CalResult start_application(const Config& cfg);
    CalResult capture(double mass_g, double t);
    CalResult remove_point(size_t index);

    CalResult start_device(const Config& cfg, double nominal_load, const std::string& unit,
                           IoBackend& io);
    CalResult device_zero(IoBackend& io, double t);
    CalResult device_reference(double mass_g, IoBackend& io, double t);

    // Liefert die neue Config; Session endet bei Erfolg.
    CalResult commit(const Config& current, Config& out);
    void cancel(const Config& cfg, IoBackend& io);

    // Pro Zyklus: Erfassung, Kommando-Polling, Stillstandserkennung
    void step(const Sample& s, IoBackend& io, const Config& cfg);

    nlohmann::json to_json() const;

    struct AppPoint {
        double raw = 0.0;
        double mass_g = 0.0;
        double stddev = 0.0;
        uint32_t n = 0;
    };
    const std::vector<AppPoint>& points() const { return points_; }
    const std::string& phase() const { return phase_; }

private:
    enum class Kind { None, Application, Device };
    void reset();
    void finish_device(IoBackend& io, const Config& cfg);

    Kind kind_ = Kind::None;
    std::string phase_;   // application: idle|capturing ; device: prepared|zero_busy|zero_done|ref_busy|done
    std::string error_;
    std::string message_;

    // Anwendung
    std::vector<AppPoint> points_;
    bool capturing_ = false;
    double cap_mass_g_ = 0.0;
    double cap_start_ = 0.0;
    double cap_duration_ = 2.0;
    uint32_t cap_n_ = 0;
    double cap_mean_ = 0.0, cap_m2_ = 0.0;

    // Klemme
    std::string device_unit_;
    double nominal_ = 0.0;      // in device_unit
    double reference_ = 0.0;    // in device_unit
    double band_ = 0.0;         // Stillstandsband in device_unit
    double stable_time_s_ = 10.0;
    double stable_ref_ = 0.0;
    double stable_since_ = -1.0;
    double last_raw_ = 0.0;
    double last_t_ = 0.0;
    double cmd_start_ = 0.0;
    double next_poll_ = 0.0;
    std::optional<double> rated_result_, zero_result_;
};

} // namespace ew
