#include "calibration_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "el3356.hpp"
#include "units.hpp"

namespace ew {

namespace {

constexpr double kPollInterval = 0.1;   ///< time between status polls of 0xFB00:02 [s]
constexpr double kCmdTimeout = 30.0;    ///< time until a command is aborted [s]
constexpr double kNotStable = -1.0;     ///< stable_since_: no standstill
constexpr uint32_t kMinCaptureSamples = 2U;  ///< minimum count for mean and standard deviation
constexpr double kSameMassG = 1e-9;     ///< same mass: replace calibration point [g]
constexpr double kNeutralScale = 1.0;   ///< scale factor during calibration (load directly)
constexpr double kNeutralGain = 1.0;    ///< gain during calibration
constexpr double kNeutralTara = 0.0;    ///< tare during calibration
constexpr size_t kErrLen = 64U;         ///< buffer for error messages

/**
 * @brief Hardware-side reason for failure of a sample.
 *
 * Without the calibration/characteristic state of the application. Texts like FAULT_TEXT in the frontend.
 *
 * @param s sample
 * @return error text or nullptr for a valid sample
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

/// @brief true if @p s is valid on the hardware side (see hw_fault()).
bool hw_valid(const Sample& s) { return hw_fault(s) == nullptr; }

/// @brief Error result with text @p e.
CalResult fail(const std::string& e) { return {false, e}; }
/// @brief Success result.
CalResult ok() { return {true, {}}; }

/**
 * @brief Writes a REAL32 parameter to 0x8000.
 * @param io backend
 * @param sub subindex in 0x8000
 * @param v value
 * @param[out] err error text
 * @return true on success
 */
bool write_f32(IoBackend& io, uint8_t sub, double v, std::string& err) {
    return io.sdo_write(el3356::kObjSettings, sub, el3356::le_f32(static_cast<float>(v)), err);
}
/**
 * @brief Writes a UINT16 object.
 * @param io backend
 * @param idx index
 * @param sub subindex
 * @param v value
 * @param[out] err error text
 * @return true on success
 */
bool write_u16(IoBackend& io, uint16_t idx, uint8_t sub, uint16_t v, std::string& err) {
    return io.sdo_write(idx, sub, el3356::le_u16(v), err);
}

} // namespace

/// @brief Ends the session and discards all intermediate results.
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

// --- Application ----------------------------------------------------------------

/**
 * @brief Starts the calibration in the application.
 * @param cfg Config (calibration.capture_time_ms)
 * @return error if a calibration is already running
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
 * @brief Starts capturing a calibration point.
 *
 * Averaging runs in step() over capture_time_ms; a point with the same mass
 * is replaced.
 *
 * @param mass_g applied mass [g], 0 = zero point
 * @param t current time [s]
 * @return error if no calibration in the application is running or a capture is already in progress
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
 * @brief Removes a captured calibration point.
 * @param index position in points()
 * @return error for an invalid index or a capture in progress
 */
CalResult CalibrationController::remove_point(size_t index) {
    if (kind_ != Kind::Application) return fail("Keine Kalibrierung in der Anwendung aktiv");
    if (capturing_) return fail("Erfassung läuft");
    if (index >= points_.size()) return fail("Punkt existiert nicht");
    points_.erase(points_.begin() + static_cast<long>(index));
    return ok();
}

// --- Terminal ---------------------------------------------------------------------

/**
 * @brief Starts the calibration in the terminal and prepares 0x8000.
 *
 * Sets scale factor 1, gain 1, tare 0, filter IIR8, gravity and nominal load.
 * If that fails, the config is written to the terminal again.
 *
 * @param cfg config (gravity, standstill criteria)
 * @param nominal_load nominal load of the load cell in @p unit
 * @param unit unit of nominal/reference load in the terminal
 * @param io backend
 * @return error for a running calibration, invalid inputs or SDO error
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
        io.write_device_config(cfg.device, e2);  // restore the initial state
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
 * @brief Triggers the zero balance (command 0x0101).
 * @param io backend
 * @param t current time [s]
 * @return error if the step is not allowed or the standstill is too short
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
 * @brief Writes the reference load and triggers the calibration (command 0x0102).
 * @param mass_g applied reference mass [g], at least 20 % of the nominal load
 * @param io backend
 * @param t current time [s]
 * @return error without prior zero balance, for too small a load or too short a standstill
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
 * @brief Reads the result (0x8000:23/25) and resets command, filter and scale factor.
 * @param io backend
 * @param cfg config with the values to restore
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

// --- Common -----------------------------------------------------------------------

/**
 * @brief Takes the result over into a new config; the session ends on success.
 * @param current current config
 * @param[out] out new config with characteristic or terminal parameters
 * @return error for fewer than 2 valid points or an incomplete terminal calibration
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
 * @brief Aborts the calibration.
 *
 * For the terminal, command 0x0000 and the parameters of the config are written back.
 *
 * @param cfg config whose terminal parameters are restored
 * @param io backend
 */
void CalibrationController::cancel(const Config& cfg, IoBackend& io) {
    if (kind_ == Kind::Device) {
        std::string err;
        write_u16(io, el3356::kObjCommand, el3356::kSubCmdRequest, el3356::kCmdNone, err);
        io.write_device_config(cfg.device, err);  // write back the parameters of the config
    }
    reset();
}

/**
 * @brief Work step per cycle: point capture, standstill detection, command polling.
 * @param s sample of the current cycle
 * @param io backend (SDO accesses)
 * @param cfg active config
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
                *it = p;  // same mass: replace point
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

    // Standstill detection on the raw value (load in device_unit, scale factor 1)
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
 * @brief State for frontend and WebSocket.
 * @return active, kind, phase, error, message, points or standstill and progress
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
