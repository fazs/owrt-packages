#include "sim_backend.hpp"

#include <cmath>
#include <cstdio>

#include "el3356.hpp"
#include "units.hpp"

namespace ew {

namespace {
constexpr double kEmpty = 2.0;               ///< Lastprofil: leer [s]
constexpr double kLoaded = 5.0;              ///< Lastprofil: belastet inkl. Einschwingen [s]
constexpr double kPeriod = kEmpty + kLoaded; ///< Periode des Lastprofils [s]
constexpr double kPi = 3.14159265358979323846;
constexpr double kRatedDev = 0.005;  ///< Kennwert +0,5 % gegenüber dem Nennwert
constexpr double kZeroDev = 0.01;    ///< Nullpunkt +1 % des Kennwerts
constexpr double kNoiseRel = 6e-5;   ///< Rauschen relativ zum Kennwert (6e-5 · Kapazität)

/**
 * @brief Rundet auf signifikante Stellen.
 * @param v Wert
 * @param digits signifikante Stellen
 * @return gerundeter Wert
 */
double round_sig(double v, int digits) {
    if (v == 0.0) return 0.0;
    const double p = std::pow(10.0, digits - 1 - std::floor(std::log10(std::fabs(v))));
    return std::round(v * p) / p;
}

/**
 * @brief Einschwingen auf eine Last: gedämpfte Schwingung 3 Hz, Abklingzeit 0,25 s.
 * @param target Endwert
 * @param tl Zeit seit Lastwechsel [s]
 * @return momentaner Wert
 */
double settle(double target, double tl) {
    // Gedämpfte Schwingung 3 Hz, Abklingzeit 0,25 s
    return target * (1.0 - std::exp(-tl / 0.25) * std::cos(2.0 * kPi * 3.0 * tl));
}
} // namespace

/**
 * @brief Leitet den simulierten Sensor aus der Konfiguration ab.
 *
 * - Kapazität = Nominal load (in calibration.device_unit)
 * - Kennwert/Nullpunkt = Nennwerte der Config mit kleiner Abweichung (kRatedDev, kZeroDev),
 *   damit eine Kalibrierung etwas zu korrigieren hat
 * - Nennwerte sind die Config-Werte gerundet (Rated output auf 2 signifikante Stellen,
 *   Zero balance auf 5 % des Rated output), damit eine in der Simulation gespeicherte
 *   Kalibrierung den Sensor nicht verschiebt
 *
 * @param cfg Konfiguration
 * @return Sensorparameter
 */
SimBackend::SimSensor SimBackend::sensor_for(const Config& cfg) {
    double rated = round_sig(cfg.device.rated_output_mv_v, 2);
    if (rated == 0.0) rated = 1.0;  // ungültige Config: Simulation trotzdem lauffähig
    const double zero_step = 0.05 * std::fabs(rated);
    const double zero = std::round(cfg.device.zero_balance / zero_step) * zero_step;
    const double cap_g = to_grams(cfg.device.nominal_load, cfg.calibration.device_unit);
    SimSensor s;
    s.capacity_kg = (cap_g > 0.0 ? cap_g : 1000.0) / 1000.0;
    s.rated_mv_v = rated * (1.0 + kRatedDev);
    s.zero_mv_v = zero + kZeroDev * rated;
    s.noise_mv_v = kNoiseRel * std::fabs(s.rated_mv_v);
    return s;
}

/**
 * @brief Baut PDO-Katalog und Prozessabbild gemäß Standard-Zuordnung auf.
 * @param seed Startwert des Zufallsgenerators (reproduzierbare Tests)
 */
SimBackend::SimBackend(unsigned seed) : rng_(seed), sensor_(sensor_for(Config{})) {
    using namespace el3356;
    pdos_ = known_pdos();
    locate_pdos(pdos_, rx_assignment(), tx_assignment());
    std::vector<std::vector<MapEntry>> rx, tx;
    uint32_t rx_bits = 0, tx_bits = 0;
    for (const auto& p : pdos_) {
        if (!p.assigned) continue;
        std::vector<MapEntry> m;
        for (const auto& e : p.entries) {
            m.push_back(e.map);
            (p.rx ? rx_bits : tx_bits) += e.map.bits;
        }
        (p.rx ? rx : tx).push_back(std::move(m));
    }
    in_ = layout_inputs(tx);
    out_ = layout_outputs(rx);
    img_in_.assign((tx_bits + 7) / 8, 0);
    img_out_.assign((rx_bits + 7) / 8, 0);
}

/**
 * @brief Übernimmt Sensor und Klemmenparameter aus der Config; bildet den PO2SO-Hook nach.
 * @param cfg Konfiguration
 * @param[out] err ungenutzt
 * @return immer true
 */
bool SimBackend::open(const Config& cfg, std::string& err) {
    sensor_ = sensor_for(cfg);
    phase_start_ = 0.0;
    target_ = load_dist_(rng_) * sensor_.capacity_kg;
    // Wirkung des PO2SO-Hooks nachbilden
    for (const auto& w : el3356::init_sequence(cfg.device))
        od_[static_cast<uint32_t>(w.index) << 8 | w.sub] = w.data;
    return write_device_config(cfg.device, err);
}

/// @copydoc IoBackend::write_device_config
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

/// @copydoc IoBackend::set_sim_load
bool SimBackend::set_sim_load(std::optional<double> grams) {
    if (grams && !(fixed_kg_ && std::fabs(*fixed_kg_ - *grams / 1000.0) < 1e-12))
        load_since_ = -1.0;  // Einschwingen beim nächsten Zyklus neu starten
    fixed_kg_ = grams ? std::optional<double>(*grams / 1000.0) : std::nullopt;
    return true;
}

/**
 * @brief Aufgelegte Masse zum Zeitpunkt @p t (feste Masse oder Lastprofil, mit Einschwingen).
 * @param t Zeit seit Start [s]
 * @return Masse [kg]
 */
double SimBackend::mass_kg(double t) {
    if (fixed_kg_) {
        if (load_since_ < 0.0) load_since_ = t;
        return settle(*fixed_kg_, t - load_since_);
    }
    double tc = t - phase_start_;
    if (tc >= kPeriod) {
        phase_start_ += kPeriod * std::floor(tc / kPeriod);
        tc = t - phase_start_;
        target_ = load_dist_(rng_) * sensor_.capacity_kg;
    }
    if (tc < kEmpty) return 0.0;
    return settle(target_, tc - kEmpty);
}

/**
 * @brief Brückensignal des Sensors inkl. Rauschen (IIR8 dämpft auf 30 %).
 * @param mass Masse [kg]
 * @return Signal [mV/V]
 */
double SimBackend::signal_mvv(double mass) {
    const double noise =
        sensor_.noise_mv_v * (filter_ == el3356::kFilterIir8 ? 0.3 : 1.0) * noise_(rng_);
    return sensor_.zero_mv_v + mass / sensor_.capacity_kg * sensor_.rated_mv_v + noise;
}

/**
 * @brief Prozessdatenzyklus: Value aus Signal und Klemmenparametern, Steady state und Overrange.
 * @param t Zeit seit Start [s]
 * @param ctrl Steuerbits (nur im Ausgangsabbild sichtbar, ohne Wirkung)
 * @param[out] out Sample
 */
void SimBackend::cycle(double t, const El3356Control& ctrl, Sample& out) {
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
    el3356::encode_inputs(img_in_.data(), in_, out.status, out.raw);
    el3356::encode_control(img_out_.data(), out_, ctrl);
}

/**
 * @brief SDO-Schreibzugriff auf das simulierte Objektverzeichnis.
 *
 * 0x8000 wirkt auf die Messung, 0xFB00:01 startet Kommandos; 0x6000, 0x16xx und 0x1Axx
 * sind nur lesbar, alle übrigen Objekte werden gespeichert.
 *
 * @copydetails IoBackend::sdo_write
 */
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
        default: break;  // übrige Parameter ohne Wirkung in der Simulation
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
    if (index == kObjInputs || (index & 0xFF00) == 0x1A00 || (index & 0xFF00) == 0x1600) {
        char b[64];
        std::snprintf(b, sizeof b, "SDO-Write 0x%04X:%02X: Objekt nur lesbar (Simulation)", index, sub);
        err = b;
        return false;
    }
    od_[static_cast<uint32_t>(index) << 8 | sub] = d;
    return true;
}

/**
 * @brief SDO-Lesezugriff auf das simulierte Objektverzeichnis.
 *
 * 0xFB00:02 schließt laufende Kommandos nach einigen Abfragen ab; Mapping und Eingänge
 * kommen aus Katalog bzw. Prozessabbild, übrige Objekte aus dem Speicher.
 *
 * @copydetails IoBackend::sdo_read
 */
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
        case kSubGain: d = le_f32(gain_); return true;
        case kSubTara: d = le_f32(tara_); return true;
        case kSubReferenceLoad: d = le_f32(reference_); return true;
        case kSubFilterMode0: d = le_u16(filter_); return true;
        default: break;
        }
    }
    // PDO-Mapping und Eingangsobjekte aus dem Katalog bzw. dem Prozessabbild
    for (const auto& p : pdos_) {
        if (index == p.index) {
            if (sub == 0) {
                d = {static_cast<uint8_t>(p.entries.size())};
                return true;
            }
            if (sub <= p.entries.size()) {
                const auto& m = p.entries[sub - 1].map;
                d = encode_value(DType::U32, static_cast<double>(static_cast<uint32_t>(m.index) << 16 |
                                                                 m.sub << 8 | m.bits));
                return true;
            }
        }
        for (const auto& e : p.entries)
            if (e.loc.present && e.map.index == index && e.map.sub == sub && index != 0) {
                d = encode_value(e.type,
                                 entry_value(p.rx ? img_out_.data() : img_in_.data(), e));
                return true;
            }
    }
    if (auto it = od_.find(static_cast<uint32_t>(index) << 8 | sub); it != od_.end()) {
        d = it->second;
        return true;
    }
    char b[64];
    std::snprintf(b, sizeof b, "SDO-Read 0x%04X:%02X nicht unterstützt (Simulation)", index, sub);
    err = b;
    return false;
}

} // namespace ew
