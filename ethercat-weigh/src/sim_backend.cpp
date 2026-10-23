#include "sim_backend.hpp"

#include <cmath>
#include <cstdio>

#include "el3356.hpp"

namespace ew {

namespace {
constexpr double kEmpty = 2.0;   // s
constexpr double kLoaded = 5.0;  // s (inkl. Einschwingen)
constexpr double kPeriod = kEmpty + kLoaded;
constexpr double kPi = 3.14159265358979323846;
constexpr double kNoiseMvv = 0.00012;  // entspricht ca. 0,3 g bei 5 kg / 2 mV/V

double settle(double target, double tl) {
    // Gedämpfte Schwingung 3 Hz, Abklingzeit 0,25 s
    return target * (1.0 - std::exp(-tl / 0.25) * std::cos(2.0 * kPi * 3.0 * tl));
}
} // namespace

SimBackend::SimBackend(unsigned seed) : rng_(seed) {}

bool SimBackend::open(const Config& cfg, std::string& err) {
    phase_start_ = 0.0;
    target_ = load_dist_(rng_);
    return write_device_config(cfg.device, err);
}

bool SimBackend::write_device_config(const DeviceConfig& dc, std::string&) {
    rated_ = static_cast<float>(dc.rated_output_mv_v);
    nominal_ = static_cast<float>(dc.nominal_load);
    zero_bal_ = static_cast<float>(dc.zero_balance);
    scale_ = static_cast<float>(dc.scale_factor);
    reference_ = static_cast<float>(dc.reference_load);
    filter_ = dc.mode0_filter_settings;
    gain_ = 1.0f;
    tara_ = 0.0f;
    return true;
}

bool SimBackend::set_sim_load(std::optional<double> grams) {
    if (grams && !(fixed_kg_ && std::fabs(*fixed_kg_ - *grams / 1000.0) < 1e-12))
        load_since_ = -1.0;  // Einschwingen beim nächsten Zyklus neu starten
    fixed_kg_ = grams ? std::optional<double>(*grams / 1000.0) : std::nullopt;
    return true;
}

double SimBackend::mass_kg(double t) {
    if (fixed_kg_) {
        if (load_since_ < 0.0) load_since_ = t;
        return settle(*fixed_kg_, t - load_since_);
    }
    double tc = t - phase_start_;
    if (tc >= kPeriod) {
        phase_start_ += kPeriod * std::floor(tc / kPeriod);
        tc = t - phase_start_;
        target_ = load_dist_(rng_);
    }
    if (tc < kEmpty) return 0.0;
    return settle(target_, tc - kEmpty);
}

double SimBackend::signal_mvv(double mass) {
    const double noise = kNoiseMvv * (filter_ == el3356::kFilterIir8 ? 0.3 : 1.0) * noise_(rng_);
    return kZeroTrue + mass / kCapacityKg * kRatedTrue + noise;
}

void SimBackend::cycle(double t, const El3356Control&, Sample& out) {
    last_signal_ = signal_mvv(mass_kg(t));
    const double load = (last_signal_ - zero_bal_) / rated_ * nominal_ * gain_ - tara_;
    const double v = load * scale_;

    // Nachbildung Steady state: Wert seit 1 s innerhalb ±0,1 % Nennlast
    const double tol = 0.001 * nominal_ * scale_;
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
    out.status.steady_state = (t - steady_since_) >= 1.0;
    out.status.overrange = std::fabs(load) > nominal_ * 1.1;
    out.status.txpdo_toggle = toggle_;
}

bool SimBackend::sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& d,
                           std::string& err) {
    using namespace el3356;
    auto f = [&] { return f32_from_le(d); };
    auto u16 = [&] { return static_cast<uint16_t>(d.size() >= 2 ? d[0] | (d[1] << 8) : 0); };
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
        default: return true;  // übrige Parameter ohne Wirkung in der Simulation
        }
    }
    if (index == kObjCommand && sub == 1) {
        cmd_ = u16();
        if (cmd_ == kCmdNone) {
            cmd_status_ = kCmdStatusDone;
            return true;
        }
        if (cmd_ != kCmdZeroBalance && cmd_ != kCmdCalibration) {
            cmd_status_ = 1;
            return true;
        }
        cmd_status_ = kCmdStatusBusy;
        cmd_busy_reads_ = 3;
        return true;
    }
    char b[64];
    std::snprintf(b, sizeof b, "SDO-Write 0x%04X:%02X nicht unterstützt (Simulation)", index, sub);
    err = b;
    return false;
}

bool SimBackend::sdo_read(uint16_t index, uint8_t sub, size_t, std::vector<uint8_t>& d,
                          std::string& err) {
    using namespace el3356;
    if (index == kObjCommand && sub == 2) {
        if (cmd_status_ == kCmdStatusBusy && --cmd_busy_reads_ <= 0) {
            if (cmd_ == kCmdZeroBalance) {
                zero_bal_ = static_cast<float>(last_signal_);
            } else if (cmd_ == kCmdCalibration) {
                const double span = last_signal_ - zero_bal_;
                rated_ = static_cast<float>(span * nominal_ / reference_);
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
        default: break;
        }
    }
    char b[64];
    std::snprintf(b, sizeof b, "SDO-Read 0x%04X:%02X nicht unterstützt (Simulation)", index, sub);
    err = b;
    return false;
}

} // namespace ew
