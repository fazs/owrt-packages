#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "io_backend.hpp"
#include "sample.hpp"

namespace ew {

/// Ergebnis eines Kommandos (Kalibrierung, Tara, Debug).
struct CalResult {
    bool ok = false;      ///< Kommando ausgeführt
    std::string error;    ///< Fehlertext bei ok == false
};

/**
 * @brief Ablaufsteuerung der Kalibrierung.
 *
 * Wird ausschließlich im Acquisition-Thread verwendet.
 *
 * Anwendung ("application"):
 * start -> capture(Masse) je Punkt (Nullpunkt = Masse 0) -> commit (>= 2 Punkte).
 * Rohwert je Punkt = Mittelwert von Value (Real) über capture_time_ms.
 *
 * Klemme ("device", Beckhoff-Verfahren "Abgleich in der Anlage"):
 * - start: Scale factor 1, Gain 1, Tara 0, Filter IIR8, Nominal load, Gravity
 * - zero: Kommando 0x0101 nach >= stable_time_s Stillstand
 * - ref: Reference load (>= 20 % Nennlast) schreiben, Kommando 0x0102
 * - danach: Kommando 0x0000, Filter und Scale factor aus der Config zurück,
 *   Rated output (0x8000:23) und Zero balance (0x8000:25) lesen
 * - commit: Ergebnis in die Config übernehmen
 */
class CalibrationController {
public:
    /// true, solange eine Kalibrierung läuft
    bool active() const { return kind_ != Kind::None; }

    CalResult start_application(const Config& cfg);
    CalResult capture(double mass_g, double t);
    CalResult remove_point(size_t index);

    CalResult start_device(const Config& cfg, double nominal_load, const std::string& unit,
                           IoBackend& io);
    CalResult device_zero(IoBackend& io, double t);
    CalResult device_reference(double mass_g, IoBackend& io, double t);

    CalResult commit(const Config& current, Config& out);
    void cancel(const Config& cfg, IoBackend& io);

    void step(const Sample& s, IoBackend& io, const Config& cfg);

    nlohmann::json to_json() const;

    /// Erfasster Stützpunkt (Kalibrierung in der Anwendung).
    struct AppPoint {
        double raw = 0.0;      ///< Mittelwert von Value (Real)
        double mass_g = 0.0;   ///< zugeordnete Masse [g]
        double stddev = 0.0;   ///< Standardabweichung des Rohwerts
        uint32_t n = 0;        ///< gemittelte Samples
    };
    /// erfasste Stützpunkte, nach Masse sortiert
    const std::vector<AppPoint>& points() const { return points_; }
    /// application: idle|capturing; device: prepared|zero_busy|zero_done|ref_busy|done
    const std::string& phase() const { return phase_; }

private:
    /// Art der laufenden Kalibrierung
    enum class Kind { None, Application, Device };
    void reset();
    void finish_device(IoBackend& io, const Config& cfg);

    Kind kind_ = Kind::None;  ///< laufende Kalibrierung
    std::string phase_;       ///< siehe phase()
    std::string error_;       ///< letzter Fehler
    std::string message_;     ///< Hinweis für die Bedienung

    // Anwendung
    std::vector<AppPoint> points_;       ///< erfasste Stützpunkte
    bool capturing_ = false;             ///< Erfassung läuft
    double cap_mass_g_ = 0.0;            ///< Masse des laufenden Punkts [g]
    double cap_start_ = 0.0;             ///< Beginn der Erfassung [s]
    double cap_duration_ = 2.0;          ///< Erfassungsdauer [s]
    uint32_t cap_n_ = 0;                 ///< bisher gemittelte Samples
    double cap_mean_ = 0.0;              ///< laufender Mittelwert (Welford)
    double cap_m2_ = 0.0;                ///< laufende Quadratsumme (Welford)

    // Klemme
    std::string device_unit_;            ///< Einheit von Nominal/Reference load
    double nominal_ = 0.0;               ///< Nennlast in device_unit
    double reference_ = 0.0;             ///< Referenzlast in device_unit
    double band_ = 0.0;                  ///< Stillstandsband in device_unit
    double stable_time_s_ = 10.0;        ///< geforderter Stillstand [s]
    double stable_ref_ = 0.0;            ///< Bezugswert der Stillstandserkennung
    double stable_since_ = -1.0;         ///< Beginn des Stillstands [s], -1 = kein Stillstand
    double last_raw_ = 0.0;              ///< letzter Rohwert
    double last_t_ = 0.0;                ///< Zeit des letzten Samples [s]
    double cmd_start_ = 0.0;             ///< Start des laufenden Kommandos [s]
    double next_poll_ = 0.0;             ///< nächste Abfrage von 0xFB00:02 [s]
    std::optional<double> rated_result_; ///< gelesenes Rated output 0x8000:23
    std::optional<double> zero_result_;  ///< gelesenes Zero balance 0x8000:25
};

} // namespace ew
