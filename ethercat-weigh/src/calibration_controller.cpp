#include "calibration_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "el3356.hpp"
#include "units.hpp"

namespace ew {

namespace {

constexpr double kPollInterval = 0.1;   ///< Zeit zwischen Statusabfragen 0xFB00:02 [s]
constexpr double kCmdTimeout = 30.0;    ///< Zeit bis zum Abbruch eines Kommandos [s]
constexpr double kNotStable = -1.0;     ///< stable_since_: kein Stillstand
constexpr uint32_t kMinCaptureSamples = 2U;  ///< Mindestanzahl für Mittelwert und Standardabweichung
constexpr double kSameMassG = 1e-9;     ///< gleiche Masse: Stützpunkt ersetzen [g]
constexpr double kNeutralScale = 1.0;   ///< Scale factor während der Kalibrierung (Last direkt)
constexpr double kNeutralGain = 1.0;    ///< Gain während der Kalibrierung
constexpr double kNeutralTara = 0.0;    ///< Tara während der Kalibrierung
constexpr size_t kErrLen = 64U;         ///< Puffer für Fehlermeldungen

/**
 * @brief Hardwareseitiger Fehlergrund eines Samples.
 *
 * Ohne Kalibrier-/Kennlinienstatus der Anwendung. Texte wie FAULT_TEXT im Frontend.
 *
 * @param s Sample
 * @return Fehlertext oder nullptr bei gültigem Sample
 */
const char* hw_fault(const Sample& s) {
    if (!s.link_ok) return "Keine Verbindung zur Klemme";
    if (!s.wkc_ok) return "Working Counter fehlerhaft";
    if (s.status.error) return "Klemme meldet Fehler (Error)";
    if (s.status.overrange) return "Messbereich überschritten (Overrange)";
    if (s.status.data_invalid) return "Messdaten ungültig (Data invalid)";
    if (s.status.calibration_in_progress) return "Selbstkalibrierung der Klemme läuft";
    return nullptr;
}

/// @brief true, wenn @p s hardwareseitig gültig ist (siehe hw_fault()).
bool hw_valid(const Sample& s) { return hw_fault(s) == nullptr; }

/// @brief Fehlerergebnis mit Text @p e.
CalResult fail(const std::string& e) { return {false, e}; }
/// @brief Erfolgsergebnis.
CalResult ok() { return {true, {}}; }

/**
 * @brief Schreibt einen REAL32-Parameter in 0x8000.
 * @param io Backend
 * @param sub Subindex in 0x8000
 * @param v Wert
 * @param[out] err Fehlertext
 * @return true bei Erfolg
 */
bool write_f32(IoBackend& io, uint8_t sub, double v, std::string& err) {
    return io.sdo_write(el3356::kObjSettings, sub, el3356::le_f32(static_cast<float>(v)), err);
}
/**
 * @brief Schreibt ein UINT16-Objekt.
 * @param io Backend
 * @param idx Index
 * @param sub Subindex
 * @param v Wert
 * @param[out] err Fehlertext
 * @return true bei Erfolg
 */
bool write_u16(IoBackend& io, uint16_t idx, uint8_t sub, uint16_t v, std::string& err) {
    return io.sdo_write(idx, sub, el3356::le_u16(v), err);
}

} // namespace

/// @brief Beendet die Session und verwirft alle Zwischenergebnisse.
void CalibrationController::reset() {
    kind_ = Kind::None;
    phase_.clear();
    error_.clear();
    message_.clear();
    points_.clear();
    capturing_ = false;
    rated_result_.reset();
    zero_result_.reset();
    stable_since_ = kNotStable;
}

// --- Anwendung ---------------------------------------------------------------

/**
 * @brief Startet die Kalibrierung in der Anwendung.
 * @param cfg Config (calibration.capture_time_ms)
 * @return Fehler, wenn bereits eine Kalibrierung läuft
 */
CalResult CalibrationController::start_application(const Config& cfg) {
    if (active()) return fail("Kalibrierung bereits aktiv");
    reset();
    kind_ = Kind::Application;
    phase_ = "idle";
    cap_duration_ = cfg.calibration.capture_time_ms / kMsPerS;
    message_ = "Nullpunkt und Referenzgewichte erfassen";
    return ok();
}

/**
 * @brief Startet die Erfassung eines Stützpunkts.
 *
 * Die Mittelung läuft in step() über capture_time_ms; ein Punkt mit gleicher Masse
 * wird ersetzt.
 *
 * @param mass_g aufgelegte Masse [g], 0 = Nullpunkt
 * @param t aktuelle Zeit [s]
 * @return Fehler, wenn keine Kalibrierung in der Anwendung läuft oder bereits erfasst wird
 */
CalResult CalibrationController::capture(double mass_g, double t) {
    if (kind_ != Kind::Application) return fail("Keine Kalibrierung in der Anwendung aktiv");
    if (capturing_) return fail("Erfassung läuft bereits");
    if (!std::isfinite(mass_g) || mass_g < 0.0) return fail("Masse muss >= 0 sein");
    capturing_ = true;
    cap_mass_g_ = mass_g;
    cap_start_ = t;
    cap_n_ = 0;
    cap_mean_ = cap_m2_ = 0.0;
    phase_ = "capturing";
    error_.clear();
    return ok();
}

/**
 * @brief Entfernt einen erfassten Stützpunkt.
 * @param index Position in points()
 * @return Fehler bei ungültigem Index oder laufender Erfassung
 */
CalResult CalibrationController::remove_point(size_t index) {
    if (kind_ != Kind::Application) return fail("Keine Kalibrierung in der Anwendung aktiv");
    if (capturing_) return fail("Erfassung läuft");
    if (index >= points_.size()) return fail("Punkt existiert nicht");
    points_.erase(points_.begin() + static_cast<long>(index));
    return ok();
}

// --- Klemme ------------------------------------------------------------------

/**
 * @brief Startet die Kalibrierung in der Klemme und bereitet 0x8000 vor.
 *
 * Setzt Scale factor 1, Gain 1, Tara 0, Filter IIR8, Gravity und Nominal load.
 * Schlägt das fehl, wird die Config wieder in die Klemme geschrieben.
 *
 * @param cfg Config (Gravity, Stillstandskriterien)
 * @param nominal_load Nennlast der Wägezelle in @p unit
 * @param unit Einheit von Nominal/Reference load in der Klemme
 * @param io Backend
 * @return Fehler bei laufender Kalibrierung, ungültigen Eingaben oder SDO-Fehler
 */
CalResult CalibrationController::start_device(const Config& cfg, double nominal_load,
                                              const std::string& unit, IoBackend& io) {
    using namespace el3356;
    if (active()) return fail("Kalibrierung bereits aktiv");
    if (!valid_unit(unit)) return fail("Einheit muss mg, g, kg oder gr sein");
    if (!(nominal_load > 0.0)) return fail("Nennlast muss > 0 sein");
    reset();

    std::string err;
    const bool w = write_u16(io, kObjCommand, kSubCmdRequest, kCmdNone, err) &&
                   write_f32(io, kSubScaleFactor, kNeutralScale, err) &&
                   write_f32(io, kSubGain, kNeutralGain, err) &&
                   write_f32(io, kSubTara, kNeutralTara, err) &&
                   write_u16(io, kObjSettings, kSubFilterMode0, kFilterIir8, err) &&
                   write_f32(io, kSubGravity, cfg.device.gravity_of_earth, err) &&
                   write_f32(io, kSubNominalLoad, nominal_load, err);
    if (!w) {
        std::string e2;
        io.write_device_config(cfg.device, e2);  // Ausgangszustand wiederherstellen
        return fail("Vorbereitung fehlgeschlagen: " + err);
    }
    kind_ = Kind::Device;
    phase_ = "prepared";
    device_unit_ = unit;
    nominal_ = nominal_load;
    band_ = cfg.calibration.stable_band_rel * nominal_load;
    stable_time_s_ = cfg.calibration.stable_time_s;
    message_ = "Waage entlasten und Stillstand abwarten, dann Nullabgleich";
    return ok();
}

/**
 * @brief Löst den Nullabgleich (Kommando 0x0101) aus.
 * @param io Backend
 * @param t aktuelle Zeit [s]
 * @return Fehler, wenn der Schritt nicht zulässig ist oder der Stillstand zu kurz ist
 */
CalResult CalibrationController::device_zero(IoBackend& io, double t) {
    if (kind_ != Kind::Device) return fail("Keine Kalibrierung in der Klemme aktiv");
    if (phase_ != "prepared" && phase_ != "zero_done")
        return fail("Nullabgleich in diesem Schritt nicht möglich");
    if (stable_since_ < 0.0 || t - stable_since_ < stable_time_s_)
        return fail("Messwert noch nicht ausreichend lange stabil");
    std::string err;
    if (!write_u16(io, el3356::kObjCommand, el3356::kSubCmdRequest, el3356::kCmdZeroBalance, err))
        return fail(err);
    phase_ = "zero_busy";
    cmd_start_ = t;
    next_poll_ = t + kPollInterval;
    error_.clear();
    return ok();
}

/**
 * @brief Schreibt die Referenzlast und löst die Kalibrierung (Kommando 0x0102) aus.
 * @param mass_g aufgelegte Referenzmasse [g], mindestens 20 % der Nennlast
 * @param io Backend
 * @param t aktuelle Zeit [s]
 * @return Fehler ohne vorherigen Nullabgleich, bei zu kleiner Last oder zu kurzem Stillstand
 */
CalResult CalibrationController::device_reference(double mass_g, IoBackend& io, double t) {
    using namespace el3356;
    if (kind_ != Kind::Device) return fail("Keine Kalibrierung in der Klemme aktiv");
    if (phase_ != "zero_done") return fail("Zuerst Nullabgleich durchführen");
    const double ref = from_grams(mass_g, device_unit_);
    if (!(ref >= kMinReferenceRatio * nominal_))
        return fail("Referenzlast muss mindestens 20 % der Nennlast betragen");
    if (stable_since_ < 0.0 || t - stable_since_ < stable_time_s_)
        return fail("Messwert noch nicht ausreichend lange stabil");
    std::string err;
    if (!write_f32(io, kSubReferenceLoad, ref, err) ||
        !write_u16(io, kObjCommand, kSubCmdRequest, kCmdCalibration, err))
        return fail(err);
    reference_ = ref;
    phase_ = "ref_busy";
    cmd_start_ = t;
    next_poll_ = t + kPollInterval;
    error_.clear();
    return ok();
}

/**
 * @brief Liest das Ergebnis (0x8000:23/25) und stellt Kommando, Filter und Scale factor zurück.
 * @param io Backend
 * @param cfg Config mit den wiederherzustellenden Werten
 */
void CalibrationController::finish_device(IoBackend& io, const Config& cfg) {
    using namespace el3356;
    std::string err;
    std::vector<uint8_t> d;
    if (io.sdo_read(kObjSettings, kSubRatedOutput, sizeof(float), d, err)) rated_result_ = f32_from_le(d);
    if (io.sdo_read(kObjSettings, kSubZeroBalance, sizeof(float), d, err)) zero_result_ = f32_from_le(d);
    write_u16(io, kObjCommand, kSubCmdRequest, kCmdNone, err);
    write_u16(io, kObjSettings, kSubFilterMode0, cfg.device.mode0_filter_settings, err);
    write_f32(io, kSubScaleFactor, cfg.device.scale_factor, err);
    if (!rated_result_ || !zero_result_) {
        error_ = "Ergebnis (0x8000:23/25) nicht lesbar";
        phase_ = "zero_done";
        return;
    }
    phase_ = "done";
    message_ = "Kalibrierung abgeschlossen; übernehmen oder abbrechen";
}

// --- Gemeinsam ---------------------------------------------------------------

/**
 * @brief Übernimmt das Ergebnis in eine neue Config; die Session endet bei Erfolg.
 * @param current aktuelle Config
 * @param[out] out neue Config mit Kennlinie bzw. Klemmenparametern
 * @return Fehler bei weniger als 2 gültigen Punkten bzw. unvollständiger Klemmenkalibrierung
 */
CalResult CalibrationController::commit(const Config& current, Config& out) {
    if (!active()) return fail("Keine Kalibrierung aktiv");
    out = current;
    if (kind_ == Kind::Application) {
        if (capturing_) return fail("Erfassung läuft");
        std::vector<CalPoint> pts;
        for (const auto& p : points_) pts.push_back({p.raw, p.mass_g});
        const auto errs = validate_points(pts);
        if (!errs.empty()) return fail(errs.front());
        out.calibration.mode = "application";
        out.calibration.points = pts;
    } else {
        if (phase_ != "done") return fail("Kalibrierung in der Klemme nicht abgeschlossen");
        out.calibration.mode = "device";
        out.calibration.device_unit = device_unit_;
        out.device.nominal_load = nominal_;
        out.device.reference_load = reference_;
        out.device.rated_output_mv_v = *rated_result_;
        out.device.zero_balance = *zero_result_;
    }
    reset();
    return ok();
}

/**
 * @brief Bricht die Kalibrierung ab.
 *
 * Bei der Klemme werden Kommando 0x0000 und die Parameter der Config zurückgeschrieben.
 *
 * @param cfg Config, deren Klemmenparameter wiederhergestellt werden
 * @param io Backend
 */
void CalibrationController::cancel(const Config& cfg, IoBackend& io) {
    if (kind_ == Kind::Device) {
        std::string err;
        write_u16(io, el3356::kObjCommand, el3356::kSubCmdRequest, el3356::kCmdNone, err);
        io.write_device_config(cfg.device, err);  // Parameter der Config zurückschreiben
    }
    reset();
}

/**
 * @brief Arbeitsschritt je Zyklus: Punkterfassung, Stillstandserkennung, Kommando-Polling.
 * @param s Sample des aktuellen Zyklus
 * @param io Backend (SDO-Zugriffe)
 * @param cfg aktive Config
 */
void CalibrationController::step(const Sample& s, IoBackend& io, const Config& cfg) {
    last_raw_ = s.raw;
    last_t_ = s.t;

    if (kind_ == Kind::Application && capturing_) {
        if (const char* why = hw_fault(s)) {
            capturing_ = false;
            phase_ = "idle";
            error_ = std::string("Erfassung abgebrochen: ") + why;
            return;
        }
        ++cap_n_;
        const double d = s.raw - cap_mean_;
        cap_mean_ += d / cap_n_;
        cap_m2_ += d * (s.raw - cap_mean_);
        if (s.t - cap_start_ >= cap_duration_ && cap_n_ >= kMinCaptureSamples) {
            AppPoint p{cap_mean_, cap_mass_g_, std::sqrt(cap_m2_ / (cap_n_ - 1)), cap_n_};
            auto it = std::find_if(points_.begin(), points_.end(), [&](const AppPoint& q) {
                return std::fabs(q.mass_g - p.mass_g) < kSameMassG;
            });
            if (it != points_.end())
                *it = p;  // gleiche Masse: Punkt ersetzen
            else
                points_.push_back(p);
            std::sort(points_.begin(), points_.end(),
                      [](const AppPoint& a, const AppPoint& b) { return a.mass_g < b.mass_g; });
            capturing_ = false;
            phase_ = "idle";
        }
        return;
    }

    if (kind_ != Kind::Device) return;

    // Stillstandserkennung auf dem Rohwert (Last in device_unit, Scale factor 1)
    if (!hw_valid(s) || std::fabs(s.raw - stable_ref_) > band_) {
        stable_ref_ = s.raw;
        stable_since_ = hw_valid(s) ? s.t : kNotStable;
    }

    if (phase_ != "zero_busy" && phase_ != "ref_busy") return;
    if (s.t < next_poll_) return;
    next_poll_ = s.t + kPollInterval;

    std::string err;
    std::vector<uint8_t> d;
    const bool zero = phase_ == "zero_busy";
    if (!io.sdo_read(el3356::kObjCommand, el3356::kSubCmdStatus, sizeof(uint8_t), d, err) || d.empty()) {
        if (s.t - cmd_start_ > kCmdTimeout) {
            error_ = "Kommandostatus nicht lesbar: " + err;
            phase_ = zero ? "prepared" : "zero_done";
        }
        return;
    }
    const uint8_t st = d[0];
    if (st == el3356::kCmdStatusBusy) {
        if (s.t - cmd_start_ > kCmdTimeout) {
            error_ = "Zeitüberschreitung des Kommandos";
            phase_ = zero ? "prepared" : "zero_done";
        }
        return;
    }
    if (st != el3356::kCmdStatusDone) {
        char b[kErrLen];
        std::snprintf(b, sizeof b, "Klemme meldet Kommandostatus %u", st);
        error_ = b;
        phase_ = zero ? "prepared" : "zero_done";
        return;
    }
    if (zero) {
        phase_ = "zero_done";
        message_ = "Referenzlast auflegen (>= 20 % Nennlast), Stillstand abwarten";
        stable_since_ = kNotStable;
    } else {
        finish_device(io, cfg);
    }
}

/**
 * @brief Zustand für Frontend und WebSocket.
 * @return active, kind, phase, error, message, Punkte bzw. Stillstand und Fortschritt
 */
nlohmann::json CalibrationController::to_json() const {
    nlohmann::json j = {{"active", active()},
                        {"kind", kind_ == Kind::Application ? "application"
                                 : kind_ == Kind::Device    ? "device"
                                                            : "none"},
                        {"phase", phase_},
                        {"error", error_},
                        {"message", message_},
                        {"raw", last_raw_}};
    if (kind_ == Kind::Application) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : points_)
            pts.push_back({{"raw", p.raw}, {"mass_g", p.mass_g}, {"stddev", p.stddev}, {"n", p.n}});
        j["points"] = pts;
        j["capturing"] = capturing_;
        j["capture_progress"] =
            capturing_ ? std::min(1.0, (last_t_ - cap_start_) / cap_duration_) : 0.0;
        j["capture_mass_g"] = cap_mass_g_;
    } else if (kind_ == Kind::Device) {
        j["device_unit"] = device_unit_;
        j["nominal_load"] = nominal_;
        j["reference_load"] = reference_;
        j["stable_s"] = stable_since_ < 0.0 ? 0.0 : last_t_ - stable_since_;
        j["stable_time_s"] = stable_time_s_;
        j["band"] = band_;
        if (rated_result_) j["rated_output_mv_v"] = *rated_result_;
        if (zero_result_) j["zero_balance"] = *zero_result_;
    }
    return j;
}

} // namespace ew
