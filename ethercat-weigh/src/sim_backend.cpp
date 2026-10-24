#include "sim_backend.hpp"

#include <cmath>
#include <cstdio>

#include "el3356.hpp"
#include "units.hpp"

namespace ew {

namespace {
constexpr double kPi = 3.14159265358979323846;  ///< Kreiszahl
constexpr double kDecimalBase = 10.0;           ///< Basis für das Runden auf signifikante Stellen
constexpr double kMassEqualKg = 1e-12;          ///< gleiche feste Masse: kein neues Einschwingen
constexpr double kNoLoadSince = -1.0;           ///< Einschwingen beim nächsten Zyklus neu starten
constexpr uint32_t kOdSubBits = 8U;             ///< Objektschlüssel: Subindex in Bit 0..7
constexpr size_t kErrLen = 64U;                 ///< Puffer für Fehlermeldungen

/**
 * @brief Rundet auf signifikante Stellen.
 * @param v Wert
 * @param digits signifikante Stellen
 * @return gerundeter Wert
 */
double round_sig(double v, int digits) {
    if (v == 0.0) return 0.0;
    const double p = std::pow(kDecimalBase, digits - 1 - std::floor(std::log10(std::fabs(v))));
    return std::round(v * p) / p;
}

/**
 * @brief Einschwingen auf eine Last: gedämpfte Schwingung (SimBackend::kSettleFrequencyHz,
 *        SimBackend::kSettleTauS).
 * @param target Endwert
 * @param tl Zeit seit Lastwechsel [s]
 * @return momentaner Wert
 */
double settle(double target, double tl) {
    return target * (1.0 - std::exp(-tl / SimBackend::kSettleTauS) *
                                std::cos(2.0 * kPi * SimBackend::kSettleFrequencyHz * tl));
}

/**
 * @brief Schlüssel eines Objekts im simulierten Objektverzeichnis.
 * @param index Objektindex
 * @param sub Subindex
 * @return index << 8 | sub
 */
uint32_t od_key(uint16_t index, uint8_t sub) {
    return static_cast<uint32_t>(index) << kOdSubBits | sub;
}
} // namespace

/**
 * @brief Leitet den simulierten Sensor aus der Konfiguration ab.
 *
 * - Kapazität = Nominal load (in calibration.device_unit)
 * - Kennwert/Nullpunkt = Nennwerte der Config mit kleiner Abweichung (kRatedDev, kZeroDev),
 *   damit eine Kalibrierung etwas zu korrigieren hat
 * - Nennwerte sind die Config-Werte gerundet (Rated output auf kRatedSigDigits signifikante
 *   Stellen, Zero balance auf kZeroStepRel des Rated output), damit eine in der Simulation
 *   gespeicherte Kalibrierung den Sensor nicht verschiebt
 *
 * @param cfg Konfiguration
 * @return Sensorparameter
 */
SimBackend::SimSensor SimBackend::sensor_for(const Config& cfg) {
    double rated = round_sig(cfg.device.rated_output_mv_v, kRatedSigDigits);
    if (rated == 0.0) rated = kFallbackRatedMvV;  // ungültige Config: Simulation trotzdem lauffähig
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
 * @brief Baut PDO-Katalog und Prozessabbild gemäß Standard-Zuordnung auf;
 *        Sensor und Objektverzeichnis aus der Default-Konfiguration.
 * @param seed Startwert des Zufallsgenerators (reproduzierbare Tests)
 */
SimBackend::SimBackend(unsigned seed) : rng_(seed), sensor_(sensor_for(Config{})) {
    using namespace el3356;
    pdos_ = known_pdos();
    locate_pdos(pdos_, rx_assignment(), tx_assignment());
    std::vector<std::vector<MapEntry>> rx, tx;
    uint32_t rx_bits = 0U, tx_bits = 0U;
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
    img_in_.assign(bytes_for_bits(tx_bits), 0U);
    img_out_.assign(bytes_for_bits(rx_bits), 0U);
    std::string unused;
    write_device_config(DeviceConfig{}, unused);
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
    for (const auto& w : el3356::init_sequence(cfg.device)) od_[od_key(w.index, w.sub)] = w.data;
    return write_device_config(cfg.device, err);
}

/**
 * @brief Übernimmt die Klemmenparameter; Gain und Tara der Klemme werden neutral gesetzt.
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
    if (tc >= kProfilePeriodS) {
        phase_start_ += kProfilePeriodS * std::floor(tc / kProfilePeriodS);
        tc = t - phase_start_;
        target_ = load_dist_(rng_) * sensor_.capacity_kg;
    }
    if (tc < kProfileEmptyS) return 0.0;
    return settle(target_, tc - kProfileEmptyS);
}

/**
 * @brief Brückensignal des Sensors inkl. Rauschen (IIR8 dämpft um kIir8NoiseFactor).
 * @param mass Masse [kg]
 * @return Signal [mV/V]
 */
double SimBackend::signal_mvv(double mass) {
    const double noise =
        sensor_.noise_mv_v * (filter_ == el3356::kFilterIir8 ? kIir8NoiseFactor : 1.0) * noise_(rng_);
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

    // Nachbildung Steady state: Wert seit kSteadyTimeS innerhalb ±kSteadyBandRel der Nennlast
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
        default: break;  // übrige Parameter ohne Wirkung in der Simulation
        }
    }
    if (index == kObjCommand && sub == kSubCmdRequest) {
        cmd_ = u16();
        if (cmd_ == kCmdNone) {
            cmd_status_ = kCmdStatusDone;
            return true;
        }
        if (cmd_ != kCmdZeroBalance && cmd_ != kCmdCalibration) {
            cmd_status_ = kCmdStatusUnknown;
            return true;
        }
        cmd_status_ = kCmdStatusBusy;
        cmd_busy_reads_ = kCmdBusyReads;
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
 * @brief SDO-Lesezugriff auf das simulierte Objektverzeichnis.
 *
 * 0xFB00:02 schließt laufende Kommandos nach kCmdBusyReads Abfragen ab; Mapping und
 * Eingänge kommen aus Katalog bzw. Prozessabbild, übrige Objekte aus dem Speicher.
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
