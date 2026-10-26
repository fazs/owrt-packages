#include "sim_backend.hpp"

#include <cmath>
#include <cstdio>

#include "el3356.hpp"
#include "units.hpp"

namespace ew {

namespace {
constexpr double kPi = 3.14159265358979323846;  ///< pi
constexpr double kDecimalBase = 10.0;           ///< base for rounding to significant digits
constexpr double kMassEqualKg = 1e-12;          ///< same fixed mass: no new settling
constexpr double kNoLoadSince = -1.0;           ///< restart settling in the next cycle
constexpr uint32_t kOdSubBits = 8U;             ///< object key: subindex in bits 0..7
constexpr size_t kErrLen = 64U;                 ///< buffer for error messages

/**
 * @brief Rounds to significant digits.
 * @param v value
 * @param digits significant digits
 * @return rounded value
 */
double round_sig(double v, int digits) {
    if (v == 0.0) return 0.0;
    const double p = std::pow(kDecimalBase, digits - 1 - std::floor(std::log10(std::fabs(v))));
    return std::round(v * p) / p;
}

/**
 * @brief Settling to a load: damped oscillation (SimBackend::kSettleFrequencyHz,
 *        SimBackend::kSettleTauS).
 * @param target final value
 * @param tl time since load change [s]
 * @return momentary value
 */
double settle(double target, double tl) {
    return target * (1.0 - std::exp(-tl / SimBackend::kSettleTauS) *
                                std::cos(2.0 * kPi * SimBackend::kSettleFrequencyHz * tl));
}

/**
 * @brief Key of an object in the simulated object dictionary.
 * @param index object index
 * @param sub subindex
 * @return index << 8 | sub
 */
uint32_t od_key(uint16_t index, uint8_t sub) {
    return static_cast<uint32_t>(index) << kOdSubBits | sub;
}
} // namespace

/**
 * @brief Derives the simulated sensor from the configuration.
 *
 * - Capacity = nominal load (in calibration.device_unit)
 * - Rated value/zero point = nominal values of the config with a small deviation (kRatedDev, kZeroDev),
 *   so that a calibration has something to correct
 * - Nominal values are the config values rounded (rated output to kRatedSigDigits significant
 *   digits, zero balance to kZeroStepRel of the rated output), so that a calibration saved in
 *   the simulation does not shift the sensor
 *
 * @param cfg configuration
 * @return sensor parameters
 */
SimBackend::SimSensor SimBackend::sensor_for(const Config& cfg) {
    double rated = round_sig(cfg.device.rated_output_mv_v, kRatedSigDigits);
    if (rated == 0.0) rated = kFallbackRatedMvV;  // invalid config: simulation still runs
    const double zero_step = kZeroStepRel * std::fabs(rated);
    const double zero = std::round(cfg.device.zero_balance / zero_step) * zero_step;
    const double cap_g = to_grams(cfg.device.nominal_load, cfg.calibration.device_unit);
    SimSensor s;
    s.capacity_kg = (cap_g > 0.0 ? cap_g : kFallbackCapacityG) / kGramsPerKg;
    s.rated_mv_v = rated * (1.0 + kRatedDev);
    s.zero_mv_v = zero + kZeroDev * rated;
    s.noise_mv_v = kNoiseRel * std::fabs(s.rated_mv_v);
    return s;
}

/**
 * @brief Builds the PDO catalog and process image according to the default assignment;
 *        sensor and object dictionary from the default configuration.
 * @param seed seed of the random generator (reproducible tests)
 */
SimBackend::SimBackend(unsigned seed) : rng_(seed), sensor_(sensor_for(Config{})) {
    build_image(DeviceConfig{});
    std::string unused;
    write_device_config(DeviceConfig{}, unused);
}

/**
 * @brief Builds the PDO catalog, position of the inputs and outputs and the process image.
 * @param dc terminal parameters (variant, filter; see el3356::pdo_assignment)
 */
void SimBackend::build_image(const DeviceConfig& dc) {
    using namespace el3356;
    const auto a = pdo_assignment(dc);
    pdos_ = known_pdos();
    locate_pdos(pdos_, a.rx, a.tx);
    std::vector<std::vector<MapEntry>> rx, tx;
    uint32_t rx_bits = 0U, tx_bits = 0U;
    // Mappings in assignment order (like 0x1C12/0x1C13)
    for (bool is_rx : {true, false})
        for (uint16_t idx : is_rx ? a.rx : a.tx)
            for (const auto& p : pdos_) {
                if (p.index != idx || p.rx != is_rx) continue;
                std::vector<MapEntry> m;
                for (const auto& e : p.entries) {
                    m.push_back(e.map);
                    (is_rx ? rx_bits : tx_bits) += e.map.bits;
                }
                (is_rx ? rx : tx).push_back(std::move(m));
            }
    in_ = layout_inputs(tx);
    out_ = layout_outputs(rx);
    img_in_.assign(bytes_for_bits(tx_bits), 0U);
    img_out_.assign(bytes_for_bits(rx_bits), 0U);
}

/**
 * @brief Takes over sensor and terminal parameters from the config; emulates the PO2SO hook.
 * @param cfg configuration
 * @param[out] err unused
 * @return always true
 */
bool SimBackend::open(const Config& cfg, std::string& err) {
    sensor_ = sensor_for(cfg);
    phase_start_ = 0.0;
    target_ = load_dist_(rng_) * sensor_.capacity_kg;
    build_image(cfg.device);
    // Emulate the effect of the PO2SO hook
    for (const auto& w : el3356::init_sequence(cfg.device)) od_[od_key(w.index, w.sub)] = w.data;
    return write_device_config(cfg.device, err);
}

/**
 * @brief Takes over the terminal parameters; gain and tare of the terminal are set neutral.
 * @copydetails IoBackend::write_device_config
 */
bool SimBackend::write_device_config(const DeviceConfig& dc, std::string&) {
    rated_ = static_cast<float>(dc.rated_output_mv_v);
    nominal_ = static_cast<float>(dc.nominal_load);
    zero_bal_ = static_cast<float>(dc.zero_balance);
    scale_ = static_cast<float>(dc.scale_factor);
    reference_ = static_cast<float>(dc.reference_load);
    filter_ = dc.mode0_filter_settings;
    gain_ = 1.0F;
    tara_ = 0.0F;
    return true;
}

/// @copydoc IoBackend::set_sim_load
bool SimBackend::set_sim_load(std::optional<double> grams) {
    if (grams && !(fixed_kg_ && std::fabs(*fixed_kg_ - *grams / kGramsPerKg) < kMassEqualKg))
        load_since_ = kNoLoadSince;
    fixed_kg_ = grams ? std::optional<double>(*grams / kGramsPerKg) : std::nullopt;
    return true;
}

/**
 * @brief Applied mass at time @p t (fixed mass or load profile, with settling).
 * @param t time since start [s]
 * @return mass [kg]
 */
double SimBackend::mass_kg(double t) {
    if (fixed_kg_) {
        if (load_since_ < 0.0) load_since_ = t;
        return settle(*fixed_kg_, t - load_since_);
    }
    double tc = t - phase_start_;
    if (tc >= kProfilePeriodS) {
        phase_start_ += kProfilePeriodS * std::floor(tc / kProfilePeriodS);
        tc = t - phase_start_;
        target_ = load_dist_(rng_) * sensor_.capacity_kg;
    }
    if (tc < kProfileEmptyS) return 0.0;
    return settle(target_, tc - kProfileEmptyS);
}

/**
 * @brief Bridge signal of the sensor incl. noise (IIR8 attenuates by kIir8NoiseFactor).
 * @param mass mass [kg]
 * @return signal [mV/V]
 */
double SimBackend::signal_mvv(double mass) {
    const double noise =
        sensor_.noise_mv_v * (filter_ == el3356::kFilterIir8 ? kIir8NoiseFactor : 1.0) * noise_(rng_);
    return sensor_.zero_mv_v + mass / sensor_.capacity_kg * sensor_.rated_mv_v + noise;
}

/**
 * @brief Process data cycle: Value from signal and terminal parameters, steady state and overrange.
 * @param t time since start [s]
 * @param ctrl control bits (only visible in the output image, without effect)
 * @param[out] out sample
 */
void SimBackend::cycle(double t, const El3356Control& ctrl, Sample& out) {
    last_signal_ = signal_mvv(mass_kg(t));
    const double load = (last_signal_ - zero_bal_) / rated_ * nominal_ * gain_ - tara_;
    const double v = load * scale_;

    // Emulation of steady state: value within ±kSteadyBandRel of the nominal load for kSteadyTimeS
    const double tol = kSteadyBandRel * nominal_ * scale_;
    if (std::fabs(v - steady_ref_) > tol) {
        steady_ref_ = v;
        steady_since_ = t;
    }
    toggle_ = !toggle_;

    out = {};
    out.t = t;
    out.raw = static_cast<float>(v);
    out.link_ok = true;
    out.wkc_ok = true;
    out.status.steady_state = (t - steady_since_) >= kSteadyTimeS;
    out.status.overrange = std::fabs(load) > nominal_ * kOverrangeFactor;
    out.status.txpdo_toggle = toggle_;
    el3356::encode_inputs(img_in_.data(), in_, out.status, out.raw);
    el3356::encode_control(img_out_.data(), out_, ctrl);
}

/**
 * @brief SDO write access to the simulated object dictionary.
 *
 * 0x8000 affects the measurement, 0xFB00:01 starts commands; 0x6000, 0x16xx and 0x1Axx
 * are read-only, all other objects are stored.
 *
 * @copydetails IoBackend::sdo_write
 */
bool SimBackend::sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& d,
                           std::string& err) {
    using namespace el3356;
    auto f = [&] { return f32_from_le(d); };
    auto u16 = [&] { return static_cast<uint16_t>(decode_value(DType::U16, d)); };
    if (index == kObjSettings) {
        switch (sub) {
        case kSubGain: gain_ = f(); return true;
        case kSubTara: tara_ = f(); return true;
        case kSubRatedOutput: rated_ = f(); return true;
        case kSubNominalLoad: nominal_ = f(); return true;
        case kSubZeroBalance: zero_bal_ = f(); return true;
        case kSubScaleFactor: scale_ = f(); return true;
        case kSubReferenceLoad: reference_ = f(); return true;
        case kSubFilterMode0: filter_ = u16(); return true;
        default: break;  // other parameters without effect in the simulation
        }
    }
    if (index == kObjCommand && sub == kSubCmdRequest) {
        cmd_ = u16();
        if (cmd_ == kCmdNone) {
            cmd_status_ = kCmdStatusDone;
            return true;
        }
        if (cmd_ != kCmdZeroBalance && cmd_ != kCmdCalibration && cmd_ != kCmdTareTemporary &&
            cmd_ != kCmdTarePermanent) {
            cmd_status_ = kCmdStatusUnknown;
            return true;
        }
        cmd_status_ = kCmdStatusBusy;
        cmd_busy_reads_ = kCmdBusyReads;
        return true;
    }
    if (index == kObjRestore && sub == kSubRestoreAll) {
        if (decode_value(DType::U32, d) == kRestoreSignature) {
            gain_ = 1.0F;
            tara_ = 0.0F;
        }
        return true;
    }
    if (index == kObjInputs || is_pdo_mapping(index)) {
        char b[kErrLen];
        std::snprintf(b, sizeof b, "SDO-Write 0x%04X:%02X: Objekt nur lesbar (Simulation)", index, sub);
        err = b;
        return false;
    }
    od_[od_key(index, sub)] = d;
    return true;
}

/**
 * @brief SDO read access to the simulated object dictionary.
 *
 * 0xFB00:02 completes running commands after kCmdBusyReads polls; mapping and
 * inputs come from the catalog or the process image, other objects from the store.
 *
 * @copydetails IoBackend::sdo_read
 */
bool SimBackend::sdo_read(uint16_t index, uint8_t sub, size_t, std::vector<uint8_t>& d,
                          std::string& err) {
    using namespace el3356;
    if (index == kObjCommand && sub == kSubCmdStatus) {
        if (cmd_status_ == kCmdStatusBusy && --cmd_busy_reads_ <= 0) {
            if (cmd_ == kCmdZeroBalance) {
                zero_bal_ = static_cast<float>(last_signal_);
            } else if (cmd_ == kCmdCalibration) {
                const double span = last_signal_ - zero_bal_;
                rated_ = static_cast<float>(span * nominal_ / reference_);
            } else if (cmd_ == kCmdTareTemporary || cmd_ == kCmdTarePermanent) {
                // Tare such that the current value results in 0
                tara_ = static_cast<float>((last_signal_ - zero_bal_) / rated_ * nominal_ * gain_);
            }
            cmd_status_ = kCmdStatusDone;
        }
        d = {cmd_status_};
        return true;
    }
    if (index == kObjSettings) {
        switch (sub) {
        case kSubRatedOutput: d = le_f32(rated_); return true;
        case kSubZeroBalance: d = le_f32(zero_bal_); return true;
        case kSubNominalLoad: d = le_f32(nominal_); return true;
        case kSubScaleFactor: d = le_f32(scale_); return true;
        case kSubGain: d = le_f32(gain_); return true;
        case kSubTara: d = le_f32(tara_); return true;
        case kSubReferenceLoad: d = le_f32(reference_); return true;
        case kSubFilterMode0: d = le_u16(filter_); return true;
        default: break;
        }
    }
    if (index == kObjInfo && sub == kSubInfoMvV) {
        d = le_f32(static_cast<float>(last_signal_));
        return true;
    }
    if (index == kObjDiag && sub > kSubCount) {
        d = {0U};  // no diagnostics in the simulation
        return true;
    }
    // PDO mapping and input objects from the catalog or the process image
    for (const auto& p : pdos_) {
        if (index == p.index) {
            if (sub == kSubCount) {
                d = {static_cast<uint8_t>(p.entries.size())};
                return true;
            }
            if (sub <= p.entries.size()) {
                const auto& m = p.entries[sub - kSubFirstEntry].map;
                d = encode_value(DType::U32, static_cast<double>(encode_map_entry(m)));
                return true;
            }
        }
        for (const auto& e : p.entries)
            if (e.loc.present && e.map.index == index && e.map.sub == sub && index != kObjGap) {
                d = encode_value(e.type, entry_value(p.rx ? img_out_.data() : img_in_.data(), e));
                return true;
            }
    }
    if (auto it = od_.find(od_key(index, sub)); it != od_.end()) {
        d = it->second;
        return true;
    }
    char b[kErrLen];
    std::snprintf(b, sizeof b, "SDO-Read 0x%04X:%02X nicht unterstützt (Simulation)", index, sub);
    err = b;
    return false;
}

} // namespace ew
