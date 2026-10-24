/**
 * @file
 * @brief Unit-Tests ohne externes Framework: ew_tests liefert Exit-Code != 0 bei Fehlern.
 *
 * Zahlenwerte stehen als benannte Konstanten: aus dem Produktivcode übernommen (Simulation,
 * Kalibrierung, Objektverzeichnis), aus der Spezifikation (Bitlage laut ESI) oder als Testdaten.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "config.hpp"
#include "el3356.hpp"
#include "measurement.hpp"
#include "calibration.hpp"
#include "calibration_controller.hpp"
#include "sim_backend.hpp"
#include "units.hpp"
#include "statistics.hpp"

namespace {

int g_fail = 0;    ///< fehlgeschlagene Prüfungen
int g_checks = 0;  ///< ausgeführte Prüfungen

/// Prüft @p cond und zählt Fehler, ohne den Test abzubrechen.
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

/// Prüft |a - b| <= eps.
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

/// Registrierter Test.
struct TestCase {
    const char* name;          ///< Name
    std::function<void()> fn;  ///< Testfunktion
};
/// @brief Liste aller registrierten Tests.
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
/// Registriert einen Test bei der statischen Initialisierung.
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
/// Definiert und registriert einen Test.
#define TEST(name)                         \
    void name();                           \
    Reg reg_##name(#name, name);           \
    void name()

// --- Gemeinsame Konstanten ---------------------------------------------------------

constexpr double kMsPerS = 1000.0;  ///< Millisekunden je Sekunde

/// Zykluszeit der Default-Konfiguration [s]
const double kCycleS = ew::Config{}.ethercat.cycle_ms / kMsPerS;

constexpr double kEps = 1e-12;       ///< Toleranz für exakt berechenbare Werte
constexpr double kEpsCoarse = 1e-9;  ///< Toleranz bei Rundung über mehrere Rechenschritte
constexpr double kEpsFloat = 1e-6;   ///< Toleranz für REAL32-Werte
constexpr double kExact = 0.0;       ///< bitgenauer Vergleich

/**
 * @brief Anzahl Zyklen für eine Dauer.
 * @param seconds Dauer [s]
 * @return gerundete Zyklenzahl bei kCycleS
 */
int cycles_for(double seconds) {
    return static_cast<int>(std::lround(seconds / kCycleS));
}

/// @brief Ausgelieferte Konfiguration (openwrt/ethercat-weigh/files/config.json).
ew::Config package_config() { return ew::load_config(EW_PACKAGE_CONFIG); }

/// @brief Kapazität des simulierten Sensors in Gramm.
double capacity_g(const ew::Config& cfg) {
    return ew::SimBackend::sensor_for(cfg).capacity_kg * ew::kGramsPerKg;
}

/// @brief Gültiges Sample mit Nettowert @p v [g] zum Zeitpunkt @p t [s].
ew::Sample valid_sample(double t, double v) {
    ew::Sample s;
    s.t = t;
    s.value = v;
    s.link_ok = true;
    s.wkc_ok = true;
    return s;
}

/// @name Messparameter und Testlasten für die Engine-Tests [g]
/// @{
constexpr double kMinLoad = 0.1;         ///< min_load
constexpr double kZeroThreshold = 0.02;  ///< zero_threshold
constexpr uint32_t kStableTimeMs = 500U; ///< stable_time_ms
constexpr double kTolerance = 0.002;     ///< tolerance
constexpr double kSigmaMax = 0.001;      ///< sigma_max
constexpr double kLoadA = 1.234;         ///< erste Last
constexpr double kLoadB = 1.0;           ///< Last nach Teilentlastung (ohne Freigabe)
constexpr double kLoadC = 2.5;           ///< Last nach vollständiger Entlastung
/// Teilentlastung: zwischen zero_threshold und min_load, gibt keine neue Messung frei
constexpr double kPartialUnload = (kZeroThreshold + kMinLoad) / 2.0;
/// Rauschamplitude: Spannweite 2 · Amplitude liegt über tolerance
constexpr double kUnstableAmplitude = 1.5 * kTolerance;
/// @}

/// @name Dauern in den Engine-Tests [s]
/// @{
constexpr double kEmptyS = 1.0;          ///< unbelastet vor der ersten Last
constexpr double kHoldS = 2.0;           ///< Last halten (deutlich länger als stable_time_ms)
constexpr double kUnloadS = 0.2;         ///< Teil- bzw. vollständige Entlastung
constexpr double kUnstableS = 5.0;       ///< instabile Last
constexpr double kShortS = 0.3;          ///< kürzer als stable_time_ms
constexpr double kSteadyBitS = 1.0;      ///< Last mit gesetztem Steady-state-Bit
/// @}

/// @brief Messparameter für die Engine-Tests.
ew::MeasurementConfig mparams() {
    ew::MeasurementConfig p;
    p.min_load = kMinLoad;
    p.zero_threshold = kZeroThreshold;
    p.stable_time_ms = kStableTimeMs;
    p.tolerance = kTolerance;
    p.sigma_max = kSigmaMax;
    return p;
}

// ---------------------------------------------------------------------------
TEST(running_stats_basic) {
    // Testreihe 1..5: Mittelwert 3, Varianz (n-1) 2,5
    const std::vector<double> series = {1.0, 2.0, 3.0, 4.0, 5.0};
    constexpr double kMean = 3.0;
    constexpr double kVariance = 2.5;
    ew::RunningStats r;
    for (double x : series) r.add(x);
    const auto s = r.summary();
    CHECK(s.n == series.size());
    CHECK_NEAR(s.mean, kMean, kEps);
    CHECK_NEAR(s.stddev, std::sqrt(kVariance), kEps);
    CHECK_NEAR(s.min, series.front(), kExact);
    CHECK_NEAR(s.max, series.back(), kExact);
    CHECK_NEAR(s.range, series.back() - series.front(), kExact);
    CHECK_NEAR(s.last, series.back(), kExact);
    r.reset();
    CHECK(r.summary().n == 0U);
}

TEST(statistics_remove_measurement) {
    constexpr uint32_t kWindow = 3U;
    const std::vector<double> all = {10.0, 12.0, 11.0, 30.0, 13.0};
    constexpr double kOutlier = 30.0;
    const std::vector<double> held = {10.0, 12.0, 11.0, 13.0};  // all ohne kOutlier
    constexpr double kHeldMean = 11.5;
    constexpr double kHeldMax = 13.0;
    constexpr double kWindowMean = 12.0;  // letzte kWindow Werte: 12, 11, 13

    // Alle Werte gehalten: Ergebnis identisch zur Neuberechnung ohne den Wert
    ew::TemporaryStatistics st(kWindow);
    for (double x : all) st.add(x);
    st.remove(kOutlier, held);
    auto j = st.to_json();
    CHECK(j["session"]["n"] == held.size());
    CHECK_NEAR(j["session"]["mean"].get<double>(), kHeldMean, kEps);
    CHECK_NEAR(j["session"]["max"].get<double>(), kHeldMax, kExact);
    CHECK(j["rolling"]["n"] == kWindow);
    CHECK_NEAR(j["rolling"]["mean"].get<double>(), kWindowMean, kEps);

    // Nur ein Teil gehalten: n/Mittelwert/Standardabweichung exakt per Welford rückwärts
    constexpr double kRemoved = 11.0;
    const std::vector<double> ref_values = {10.0, 12.0, 30.0, 13.0};  // all ohne kRemoved
    const std::vector<double> partly_held = {30.0, 13.0};
    ew::RunningStats r, ref;
    for (double x : all) r.add(x);
    for (double x : ref_values) ref.add(x);
    r.remove(kRemoved, partly_held);
    CHECK(r.summary().n == ref_values.size());
    CHECK_NEAR(r.summary().mean, ref.summary().mean, kEps);
    CHECK_NEAR(r.summary().stddev, ref.summary().stddev, kEps);
    CHECK(r.summary().last == partly_held.back());
}

TEST(rolling_stats_window) {
    constexpr uint32_t kWindow = 3U;
    constexpr uint32_t kSmallerWindow = 2U;
    const std::vector<double> values = {10.0, 1.0, 2.0, 3.0};
    constexpr double kWindowMean = 2.0;         // 1, 2, 3
    constexpr double kSmallerWindowMean = 2.5;  // 2, 3
    ew::RollingStats r(kWindow);
    for (double x : values) r.add(x);
    const auto s = r.summary();
    CHECK(s.n == kWindow);
    CHECK_NEAR(s.mean, kWindowMean, kEps);
    CHECK_NEAR(s.max, values.back(), kExact);
    r.resize(kSmallerWindow);
    CHECK(r.summary().n == kSmallerWindow);
    CHECK_NEAR(r.summary().mean, kSmallerWindowMean, kEps);
}

TEST(measurement_single_capture_and_rearm) {
    const auto p = mparams();
    ew::MeasurementEngine e(p);
    double t = 0.0;
    int captures = 0;
    double captured = 0.0;
    const double window_s = p.stable_time_ms / kMsPerS;
    for (int i = 0; i < cycles_for(kEmptyS); ++i, t += kCycleS) CHECK(!e.process(valid_sample(t, 0.0)));
    CHECK(e.state() == ew::MeasState::Idle);
    for (int i = 0; i < cycles_for(kHoldS); ++i, t += kCycleS) {
        if (auto m = e.process(valid_sample(t, kLoadA))) {
            ++captures;
            captured = m->value;
            CHECK(m->n >= static_cast<uint32_t>(cycles_for(window_s)));
            CHECK(m->settle_ms >= p.stable_time_ms - kEpsFloat);
        }
    }
    CHECK(captures == 1);
    CHECK_NEAR(captured, kLoadA, kEps);
    CHECK(e.state() == ew::MeasState::WaitUnload);
    // Teilentlastung über zero_threshold: keine Freigabe
    for (int i = 0; i < cycles_for(kUnloadS); ++i, t += kCycleS) e.process(valid_sample(t, kPartialUnload));
    for (int i = 0; i < cycles_for(kHoldS); ++i, t += kCycleS)
        if (e.process(valid_sample(t, kLoadB))) ++captures;
    CHECK(captures == 1);
    // Entlastung unter zero_threshold: Freigabe
    for (int i = 0; i < cycles_for(kUnloadS); ++i, t += kCycleS) e.process(valid_sample(t, 0.0));
    CHECK(e.armed());
    for (int i = 0; i < cycles_for(kHoldS); ++i, t += kCycleS)
        if (auto m = e.process(valid_sample(t, kLoadC))) {
            ++captures;
            CHECK_NEAR(m->value, kLoadC, kEps);
        }
    CHECK(captures == 2);
}

TEST(measurement_unstable_no_capture) {
    const auto p = mparams();
    ew::MeasurementEngine e(p);
    double t = 0.0;
    int captures = 0;
    for (int i = 0; i < cycles_for(kUnstableS); ++i, t += kCycleS) {
        const double v = kLoadB + ((i % 2) != 0 ? kUnstableAmplitude : -kUnstableAmplitude);
        if (e.process(valid_sample(t, v))) ++captures;
    }
    CHECK(captures == 0);
    CHECK(e.state() == ew::MeasState::Settling);
    CHECK(e.window().range > p.tolerance);
}

TEST(measurement_fault_clears_window) {
    ew::MeasurementEngine e(mparams());
    double t = 0.0;
    const int n = cycles_for(kShortS);
    for (int i = 0; i < n; ++i, t += kCycleS) e.process(valid_sample(t, kLoadB));
    CHECK(e.window().n == static_cast<uint32_t>(n));
    auto s = valid_sample(t, kLoadB);
    s.status.calibration_in_progress = true;
    CHECK(!e.process(s));
    CHECK(e.state() == ew::MeasState::Fault);
    CHECK(e.fault_reason() == "calibration");
    CHECK(e.window().n == 0U);
    auto w = valid_sample(t, kLoadB);
    w.wkc_ok = false;
    e.process(w);
    CHECK(e.fault_reason() == "wkc");
}

TEST(measurement_device_steady_state_required) {
    auto p = mparams();
    p.use_device_steady_state = true;
    ew::MeasurementEngine e(p);
    double t = 0.0;
    int captures = 0;
    for (int i = 0; i < cycles_for(kHoldS); ++i, t += kCycleS)
        if (e.process(valid_sample(t, kLoadB))) ++captures;
    CHECK(captures == 0);
    for (int i = 0; i < cycles_for(kSteadyBitS); ++i, t += kCycleS) {
        auto s = valid_sample(t, kLoadB);
        s.status.steady_state = true;
        if (e.process(s)) ++captures;
    }
    CHECK(captures == 1);
}

TEST(config_validation_and_json) {
    constexpr double kNegativeRated = -1.5;   // negatives Vorzeichen zulässig
    constexpr double kJsonMinLoad = 300.0;
    constexpr double kChangedTolerance = 0.01;
    constexpr double kNominalFactor = 2.0;

    ew::Config c;
    CHECK(c.validate().empty());
    c.measurement.zero_threshold = c.measurement.min_load * kNominalFactor;  // Nullschwelle > Mindestlast
    CHECK(!c.validate().empty());

    ew::Config cr;
    cr.device.rated_output_mv_v = kNegativeRated;
    CHECK(cr.validate().empty());
    cr.device.rated_output_mv_v = 0.0;
    CHECK(!cr.validate().empty());

    const nlohmann::json j = {{"measurement", {{"min_load", kJsonMinLoad}}}, {"unknown", 1}};
    const auto c2 = ew::config_from_json(j);
    CHECK_NEAR(c2.measurement.min_load, kJsonMinLoad, kEps);
    CHECK(c2.ethercat.cycle_ms == ew::Config{}.ethercat.cycle_ms);  // Default bleibt erhalten
    const auto c3 = ew::config_from_json(nlohmann::json(c2));
    CHECK(nlohmann::json(c3) == nlohmann::json(c2));

    ew::Config a, b;
    b.measurement.tolerance = kChangedTolerance;
    CHECK(!ew::requires_restart(a, b));
    b.ethercat.interface = a.ethercat.interface + "-other";
    CHECK(ew::requires_restart(a, b));
    b = a;
    b.device.nominal_load = a.device.nominal_load * kNominalFactor;
    CHECK(ew::device_config_changed(a, b));
}

/// @name Bitlage in RMB Status 0x1A00 und RMB Value (Real) 0x1A02 laut ESI
/// @{
constexpr uint32_t kBitOverrange = 1U;      ///< 0x6000:02
constexpr uint32_t kBitError = 6U;          ///< 0x6000:07
constexpr uint32_t kBitSteadyState = 8U;    ///< 0x6000:09
constexpr uint32_t kBitSyncError = 13U;     ///< 0x6000:0E bzw. 0x1C32:20
constexpr uint32_t kBitToggle = 15U;        ///< 0x6000:10 bzw. 0x1800:09
constexpr uint32_t kBitValueReal = 16U;     ///< 0x6000:12, direkt hinter 0x1A00 (2 Byte)
constexpr uint32_t kBitTare = 4U;           ///< 0x7000:05 in RMB Control 0x1600
constexpr uint32_t kBitsPerByte = 8U;       ///< Bits je Byte
/// @}

/**
 * @brief Mapping von RMB Status 0x1A00 laut ESI; Lücken mit ihrer Breite.
 * @param sync_error Eintrag für Sync error (Revision bis 0x0014: 0x1C32:20)
 * @param toggle Eintrag für TxPDO Toggle (Revision bis 0x0014: 0x1800:09)
 * @return Mapping-Einträge
 */
std::vector<ew::el3356::MapEntry> status_mapping(ew::el3356::MapEntry sync_error, ew::el3356::MapEntry toggle) {
    using namespace ew::el3356;
    auto in = [](uint8_t sub) { return MapEntry{kObjInputs, sub, kBitsBool}; };
    auto gap = [](uint8_t bits) { return MapEntry{kObjGap, kSubCount, bits}; };
    return {gap(1U), in(kSubInOverrange), gap(1U), in(kSubInDataInvalid), gap(2U), in(kSubInError),
            in(kSubInCalibration), in(kSubInSteadyState), gap(4U), sync_error, gap(1U), toggle};
}

TEST(el3356_layout_and_decode) {
    using namespace ew::el3356;
    constexpr float kValueReal = 3.5F;
    const auto status = status_mapping({kObjInputs, kSubInSyncError, kBitsBool},
                                       {kObjInputs, kSubInToggle, kBitsBool});
    const std::vector<MapEntry> value = {{kObjInputs, kSubInValueReal, kBitsDint}};
    const auto l = layout_inputs({status, value});
    CHECK(l.overrange.present && l.overrange.bit_offset == kBitOverrange);
    CHECK(l.error.bit_offset == kBitError);
    CHECK(l.steady_state.bit_offset == kBitSteadyState);
    CHECK(l.toggle.bit_offset == kBitToggle);
    CHECK(l.value_real.bit_offset == kBitValueReal && l.value_real.bit_len == kBitsDint);

    std::vector<uint8_t> img(bytes_for_bits(kBitValueReal + kBitsDint), 0U);
    img[kBitSteadyState / kBitsPerByte] |= static_cast<uint8_t>(1U << (kBitSteadyState % kBitsPerByte));
    std::memcpy(img.data() + kBitValueReal / kBitsPerByte, &kValueReal, sizeof kValueReal);
    const auto s = decode_status(img.data(), l);
    CHECK(s.steady_state && !s.overrange && !s.error);
    CHECK_NEAR(decode_value_real(img.data(), l), kValueReal, kExact);

    const auto ol = layout_outputs({{{kObjControl, kSubCtrlStartCalibration, kBitsBool},
                                     {kObjControl, kSubCtrlDisableCalibration, kBitsBool},
                                     {kObjControl, kSubCtrlInputFreeze, kBitsBool},
                                     {kObjControl, kSubCtrlSampleMode, kBitsBool},
                                     {kObjControl, kSubCtrlTare, kBitsBool},
                                     {kObjGap, kSubCount, kBitsUint - kBitTare - kBitsBool}}});
    std::vector<uint8_t> out(bytes_for_bits(kBitsUint), 0U);
    ew::El3356Control c;
    c.tare = true;
    encode_control(out.data(), ol, c);
    CHECK(out[0] == (1U << kBitTare));
    c.tare = false;
    encode_control(out.data(), ol, c);
    CHECK(out[0] == 0U);

    const MapEntry me{kObjInputs, kSubInValueReal, kBitsDint};
    const auto back = decode_map_entry(encode_map_entry(me));
    CHECK(back.index == me.index && back.sub == me.sub && back.bits == me.bits);

    // Reihenfolge der Parameter in 0x8000 laut Initialisierungssequenz
    const std::vector<uint8_t> expected = {kSubMode0EnableFilter, kSubMode0EnableAverager, kSubFilterMode0,
                                           kSubRatedOutput,       kSubNominalLoad,         kSubZeroBalance,
                                           kSubGravity,           kSubScaleFactor,         kSubReferenceLoad,
                                           kSubSteadyStateWindow, kSubSteadyStateTolerance, kSubCalibrationInterval};
    const auto writes = settings_writes(ew::DeviceConfig{});
    CHECK(writes.size() == expected.size());
    for (size_t i = 0U; i < std::min(writes.size(), expected.size()); ++i)
        CHECK(writes[i].index == kObjSettings && writes[i].sub == expected[i]);
}

TEST(el3356_debug_catalog_and_init_sequence) {
    using namespace ew::el3356;
    constexpr float kValueReal = -12.25F;
    constexpr double kLargeU64 = 1e12;
    constexpr int16_t kNegativeI16 = -2;
    constexpr uint8_t kSubCtrlInvalid = kSubCtrlTare + 1U;  // erster nicht belegter Subindex
    constexpr size_t kAssignFrame = 2U;  // je Zuordnung: Anzahl löschen + Anzahl setzen

    auto pdos = known_pdos();
    locate_pdos(pdos, rx_assignment(), tx_assignment());
    const std::vector<uint16_t> all = {kPdoControl,       kPdoFilterFrequency, kPdoStatus,        kPdoValueInt,
                                       kPdoValueReal,     kPdoTimestamp,       kPdoAiStandardCh1, kPdoAiCompactCh1,
                                       kPdoAiStandardCh2, kPdoAiCompactCh2};
    CHECK(pdos.size() == all.size());
    const PdoInfo* status = nullptr;
    const PdoInfo* real = nullptr;
    for (const auto& p : pdos) {
        CHECK(std::find(all.begin(), all.end(), p.index) != all.end());
        if (p.index == kPdoStatus) status = &p;
        if (p.index == kPdoValueReal) real = &p;
        if (p.index == kPdoValueInt) CHECK(!p.assigned);
    }
    CHECK(status && status->assigned && real && real->assigned);
    uint32_t status_bits = 0U;
    for (const auto& e : status->entries) status_bits += e.map.bits;
    CHECK(real->entries.front().loc.bit_offset == status_bits && real->entries.front().type == DType::F32);

    // Statusbits + Value (Real) schreiben und über Layout und Katalog zurücklesen
    std::vector<std::vector<MapEntry>> tx;
    for (const auto* p : {status, real}) {
        std::vector<MapEntry> m;
        for (const auto& e : p->entries) m.push_back(e.map);
        tx.push_back(m);
    }
    const auto il = layout_inputs(tx);
    std::vector<uint8_t> img(bytes_for_bits(status_bits + kBitsDint), 0U);
    ew::El3356Status st;
    st.steady_state = true;
    st.txpdo_toggle = true;
    encode_inputs(img.data(), il, st, kValueReal);
    const auto back = decode_status(img.data(), il);
    CHECK(back.steady_state && back.txpdo_toggle && !back.error);
    CHECK_NEAR(entry_value(img.data(), real->entries.front()), kValueReal, kExact);
    const auto steady = std::find_if(status->entries.begin(), status->entries.end(), [](const PdoEntry& e) {
        return e.map.index == kObjInputs && e.map.sub == kSubInSteadyState;
    });
    CHECK(steady != status->entries.end() && entry_value(img.data(), *steady) == 1.0);

    // Bis Revision 0x0014 (ESI): Sync error 0x1C32:20, TxPDO Toggle 0x1800:09
    const auto old = layout_inputs({status_mapping({kObjSmOutParam, kSubSmOutSyncError, kBitsBool},
                                                   {kObjTxPdoPar, kSubTxPdoParToggle, kBitsBool})});
    CHECK(old.sync_error.present && old.sync_error.bit_offset == kBitSyncError);
    CHECK(old.toggle.present && old.toggle.bit_offset == kBitToggle);

    // Typ-Kodierung
    const auto pdo_bytes = encode_value(DType::U16, kPdoValueReal);
    CHECK(pdo_bytes.size() == sizeof(uint16_t) && decode_value(DType::U16, pdo_bytes) == kPdoValueReal);
    CHECK(decode_value(DType::I16, encode_value(DType::I16, kNegativeI16)) == kNegativeI16);
    const double gravity = ew::DeviceConfig{}.gravity_of_earth;
    CHECK_NEAR(decode_value(DType::F32, encode_value(DType::F32, gravity)), gravity, kEpsFloat);
    DType t;
    CHECK(dtype_from_name("i32", t) && t == DType::I32 && !dtype_from_name("x", t));
    CHECK(decode_value(DType::U64, encode_value(DType::U64, kLargeU64)) == kLargeU64);

    ew::El3356Control c;
    CHECK(control_bit(c, kSubCtrlTare) == &c.tare && control_bit(c, kSubCtrlInvalid) == nullptr);

    ew::DeviceConfig dc;
    const size_t rx_len = rx_assignment().size() + kAssignFrame;
    const size_t tx_len = tx_assignment().size() + kAssignFrame;
    const auto seq = init_sequence(dc);
    CHECK(seq.size() == rx_len + tx_len + settings_writes(dc).size());
    CHECK(seq.front().index == kSm2Assign && seq.front().sub == kSubCount && seq.front().pre_op_only);
    const auto pos = std::find(tx_assignment().begin(), tx_assignment().end(), kPdoValueReal) - tx_assignment().begin();
    const auto real_assign = std::find_if(seq.begin(), seq.end(), [&](const SdoWrite& w) {
        return w.index == kSm3Assign && w.sub == kSubFirstEntry + pos;
    });
    CHECK(real_assign != seq.end() && decode_value(DType::U16, real_assign->data) == kPdoValueReal);
    dc.write_on_start = false;
    CHECK(init_sequence(dc).size() == rx_len + tx_len);
}

/// @name Simulation
/// @{
constexpr unsigned kSeedEndToEnd = 42U;    ///< Zufallsstart: Lastprofil
constexpr unsigned kSeedAppCal = 7U;       ///< Zufallsstart: Kalibrierung in der Anwendung
constexpr unsigned kSeedAbort = 3U;        ///< Zufallsstart: Abbruch durch Overrange
constexpr unsigned kSeedDeviceCal = 9U;    ///< Zufallsstart: Kalibrierung in der Klemme
constexpr int kEndToEndPeriods = 4;        ///< Perioden des Lastprofils im Ende-zu-Ende-Test
constexpr double kPeriodMargin = 0.3;      ///< zusätzliche Laufzeit (Anteil einer Periode)
constexpr double kLoadMargin = 0.01;       ///< Toleranz der erfassten Last (Anteil der Kapazität)
/// Einschwingen der Simulation: 10 Zeitkonstanten (Restschwingung < 5e-5)
constexpr double kSettleS = 10.0 * ew::SimBackend::kSettleTauS;
constexpr double kMarginS = 0.5;           ///< Sicherheitszuschlag auf Wartezeiten [s]
constexpr double kCmdWaitS = 1.0;          ///< > SimBackend::kCmdBusyReads Statusabfragen
constexpr double kAccuracyRel = 1e-3;      ///< Genauigkeit nach Kalibrierung (Anteil der Kapazität)
constexpr double kZeroAccuracyRel = 1e-4;  ///< Genauigkeit Zero balance (Anteil des Kennwerts)
constexpr double kRatedAccuracyRel = 2e-3; ///< Genauigkeit Rated output (Anteil des Kennwerts)
/// @}

/// @name Testlasten als Anteil der Kapazität
/// @{
constexpr double kRefRatio = 2.0 * ew::CalibrationController::kMinReferenceRatio;  ///< zulässige Referenz
constexpr double kSmallRefRatio = ew::CalibrationController::kMinReferenceRatio / 2.0;  ///< zu kleine Referenz
constexpr double kCheckRatio = 0.6;      ///< Prüflast nach der Kalibrierung in der Klemme
constexpr double kAppCheckRatio = 0.2;   ///< Prüflast nach der Kalibrierung in der Anwendung
constexpr double kOverloadRatio = 1.5;   ///< Last im Overrange
static_assert(kOverloadRatio > ew::SimBackend::kOverrangeFactor, "Last muss im Overrange liegen");
static_assert(kCheckRatio < 1.0 && kAppCheckRatio < 1.0, "Prüflasten innerhalb der Nennlast");
/// @}

TEST(simulation_end_to_end) {
    ew::Config cfg = package_config();
    const auto sensor = ew::SimBackend::sensor_for(cfg);
    const double cap = capacity_g(cfg);
    cfg.device.zero_balance = sensor.zero_mv_v;  // Klemme als kalibriert annehmen
    cfg.device.rated_output_mv_v = sensor.rated_mv_v;
    ew::SimBackend sim(kSeedEndToEnd);
    std::string err;
    CHECK(sim.open(cfg, err));
    const auto ch = ew::Characteristic::from_config(cfg.calibration, cfg.device);
    CHECK(ch.valid());
    ew::MeasurementEngine e(cfg.measurement);
    int captures = 0;
    ew::El3356Control ctrl;
    const int n = cycles_for((kEndToEndPeriods + kPeriodMargin) * ew::SimBackend::kProfilePeriodS);
    for (int i = 0; i < n; ++i) {
        ew::Sample s;
        sim.cycle(i * kCycleS, ctrl, s);
        s.value = s.gross = *ch.to_grams(s.raw);
        if (auto m = e.process(s)) {
            ++captures;
            CHECK(m->value > (ew::SimBackend::kLoadFractionMin - kLoadMargin) * cap &&
                  m->value < (ew::SimBackend::kLoadFractionMax + kLoadMargin) * cap);
            CHECK(m->range <= cfg.measurement.tolerance);
        }
    }
    CHECK(captures >= kEndToEndPeriods - 1 && captures <= kEndToEndPeriods + 1);
}

TEST(units_conversion) {
    // Unabhängige Referenzwerte: 1 gr = 64,79891 mg (Definition), 1 kg = 1000 g, 1 g = 1000 mg
    constexpr double kGrainG = 0.06479891;
    constexpr double kThousandGrainG = 64.79891;
    constexpr double kThousand = 1000.0;
    constexpr double kKg = 2.5;
    CHECK_NEAR(ew::to_grams(1.0, "gr"), kGrainG, kEps);
    CHECK_NEAR(ew::from_grams(kThousandGrainG, "gr"), kThousand, kEpsCoarse);
    CHECK_NEAR(ew::to_grams(kKg, "kg"), kKg * kThousand, kEps);
    CHECK_NEAR(ew::from_grams(1.0, "mg"), kThousand, kEpsCoarse);
    CHECK(!ew::valid_unit("lb"));
}

TEST(characteristic_device) {
    constexpr double kScaleKg = 1000.0;  // Value = Last[kg] · 1000 -> Value entspricht Gramm
    constexpr double kRaw = 1234.0;
    constexpr double kGrainRaw = 1000.0;
    ew::CalibrationConfig cal;
    ew::DeviceConfig dev;
    cal.mode = "device";
    cal.device_unit = "kg";
    dev.scale_factor = kScaleKg;
    const auto c = ew::Characteristic::from_config(cal, dev);
    CHECK(c.valid());
    CHECK_NEAR(*c.to_grams(kRaw), kRaw * ew::kGramsPerKg / kScaleKg, kEpsCoarse);
    cal.device_unit = "gr";
    dev.scale_factor = 1.0;
    CHECK_NEAR(*ew::Characteristic::from_config(cal, dev).to_grams(kGrainRaw), kGrainRaw * ew::kGramsPerGr,
               kEpsCoarse);
}

TEST(characteristic_application_piecewise) {
    // Stützpunkte (raw, g), unsortiert; Steigung 0,5 g bis raw 1000, danach 0,55 g
    const std::vector<ew::CalPoint> pts = {{1000.0, 500.0}, {0.0, 0.0}, {3000.0, 1600.0}};
    struct Expect {
        double raw;  ///< Rohwert
        double g;    ///< erwartete Masse
    };
    const std::vector<Expect> expect = {
        {0.0, 0.0}, {500.0, 250.0}, {2000.0, 1050.0},
        {-100.0, -50.0},   // Extrapolation erstes Segment
        {4000.0, 2150.0},  // Extrapolation letztes Segment
    };
    ew::CalibrationConfig cal;
    cal.mode = "application";
    cal.points = pts;
    const auto c = ew::Characteristic::from_config(cal, ew::DeviceConfig{});
    CHECK(c.valid());
    for (const auto& x : expect) CHECK_NEAR(*c.to_grams(x.raw), x.g, kEps);
    cal.points = {pts.front()};
    CHECK(!ew::Characteristic::from_config(cal, ew::DeviceConfig{}).valid());
    CHECK(!ew::Characteristic::from_config(cal, ew::DeviceConfig{}).to_grams(pts.front().raw));
}

TEST(validate_points_rules) {
    // Stützpunkte (raw, g) als Testdaten
    CHECK(!ew::validate_points({{0, 0}}).empty());                     // nur ein Punkt
    CHECK(!ew::validate_points({{0, 0}, {0, 100}}).empty());           // gleicher Rohwert
    CHECK(!ew::validate_points({{0, 0}, {1, 100}, {2, 50}}).empty());  // nicht monoton
    CHECK(ew::validate_points({{0, 0}, {-1, 100}}).empty());           // fallende Kennlinie zulässig
    CHECK(ew::validate_points({{0, 0}, {1, 100}, {3, 300}}).empty());
}

/**
 * @brief Führt Zyklen (kCycleS) mit Simulation und Kalibriersteuerung aus.
 * @param sim Simulation
 * @param cc Kalibriersteuerung
 * @param cfg Konfiguration
 * @param t Startzeit [s]
 * @param seconds Dauer [s]
 * @param[out] last letztes Sample (optional)
 * @return Endzeit [s]
 */
static double run_cycles(ew::SimBackend& sim, ew::CalibrationController& cc, const ew::Config& cfg,
                         double t, double seconds, ew::Sample* last = nullptr) {
    ew::El3356Control ctrl;
    const int n = cycles_for(seconds);
    for (int i = 0; i < n; ++i, t += kCycleS) {
        ew::Sample s;
        sim.cycle(t, ctrl, s);
        cc.step(s, sim, cfg);
        if (last) *last = s;
    }
    return t;
}

/// @brief Wartezeit für einen Kalibrierpunkt: Erfassungszeit der Config plus Zuschlag [s].
double capture_wait_s(const ew::Config& cfg) {
    return cfg.calibration.capture_time_ms / kMsPerS + kMarginS;
}

TEST(calibration_application_flow) {
    constexpr double kRefLoadRatio = 0.4;  // zweiter Stützpunkt
    const ew::Config cfg = package_config();
    const double cap = capacity_g(cfg);
    ew::SimBackend sim(kSeedAppCal);
    std::string err;
    CHECK(sim.open(cfg, err));
    ew::CalibrationController cc;
    CHECK(cc.start_application(cfg).ok);
    double t = 0.0;
    sim.set_sim_load(0.0);
    t = run_cycles(sim, cc, cfg, t, kSettleS);
    CHECK(cc.capture(0.0, t).ok);
    t = run_cycles(sim, cc, cfg, t, capture_wait_s(cfg));
    CHECK(cc.points().size() == 1U);
    const double m2 = kRefLoadRatio * cap;
    const double m2_gr = ew::from_grams(m2, "gr");  // Masse in Grain angeben
    sim.set_sim_load(m2);
    t = run_cycles(sim, cc, cfg, t, kSettleS);
    CHECK(cc.capture(ew::to_grams(m2_gr, "gr"), t).ok);
    t = run_cycles(sim, cc, cfg, t, capture_wait_s(cfg));
    CHECK(cc.points().size() == 2U);
    CHECK(cc.remove_point(cc.points().size()).ok == false);  // Index hinter dem letzten Punkt
    ew::Config out;
    CHECK(cc.commit(cfg, out).ok);
    CHECK(!cc.active());
    CHECK(out.calibration.mode == "application");
    CHECK(out.calibration.points.size() == 2U);

    const auto ch = ew::Characteristic::from_config(out.calibration, out.device);
    sim.set_sim_load(kAppCheckRatio * cap);
    ew::Sample s;
    t = run_cycles(sim, cc, cfg, t, kSettleS + kMarginS, &s);
    CHECK_NEAR(*ch.to_grams(s.raw), kAppCheckRatio * cap, kAccuracyRel * cap);
}

TEST(calibration_capture_abort_reason) {
    const ew::Config cfg = package_config();
    const double cap = capacity_g(cfg);
    ew::SimBackend sim(kSeedAbort);
    std::string err;
    CHECK(sim.open(cfg, err));
    ew::CalibrationController cc;
    CHECK(cc.start_application(cfg).ok);
    sim.set_sim_load(0.0);
    double t = run_cycles(sim, cc, cfg, 0.0, kCmdWaitS);
    CHECK(cc.capture(kOverloadRatio * cap, t).ok);
    sim.set_sim_load(kOverloadRatio * cap);
    t = run_cycles(sim, cc, cfg, t, capture_wait_s(cfg));
    CHECK(cc.points().empty());
    CHECK(cc.to_json()["error"] == "Erfassung abgebrochen: Messbereich überschritten (Overrange)");
}

TEST(calibration_device_flow) {
    const ew::Config cfg = package_config();  // Klemme mit Nennwerten der Config (unkalibriert)
    const auto sensor = ew::SimBackend::sensor_for(cfg);
    const double cap = capacity_g(cfg);
    const double stable_s = cfg.calibration.stable_time_s;
    ew::SimBackend sim(kSeedDeviceCal);
    std::string err;
    CHECK(sim.open(cfg, err));
    ew::CalibrationController cc;
    CHECK(cc.start_device(cfg, cap, "g", sim).ok);
    double t = 0.0;
    sim.set_sim_load(0.0);
    t = run_cycles(sim, cc, cfg, t, kSettleS);
    CHECK(!cc.device_zero(sim, t).ok);       // noch nicht stable_time_s stabil
    t = run_cycles(sim, cc, cfg, t, stable_s);
    CHECK(cc.device_zero(sim, t).ok);
    t = run_cycles(sim, cc, cfg, t, kCmdWaitS);
    CHECK(cc.phase() == "zero_done");
    sim.set_sim_load(kSmallRefRatio * cap);
    t = run_cycles(sim, cc, cfg, t, kSettleS + stable_s);
    CHECK(!cc.device_reference(kSmallRefRatio * cap, sim, t).ok);  // < kMinReferenceRatio
    sim.set_sim_load(kRefRatio * cap);
    t = run_cycles(sim, cc, cfg, t, kSettleS + stable_s);
    CHECK(cc.device_reference(kRefRatio * cap, sim, t).ok);
    t = run_cycles(sim, cc, cfg, t, kCmdWaitS);
    CHECK(cc.phase() == "done");
    ew::Config out;
    CHECK(cc.commit(cfg, out).ok);
    CHECK(out.calibration.mode == "device" && out.calibration.device_unit == "g");
    // Ergebnis trifft den simulierten Sensor (nicht die Nennwerte der Config)
    const double rated = std::fabs(sensor.rated_mv_v);
    CHECK(std::fabs(sensor.rated_mv_v - cfg.device.rated_output_mv_v) > ew::SimBackend::kRatedDev / 2.0 * rated);
    CHECK_NEAR(out.device.zero_balance, sensor.zero_mv_v, kZeroAccuracyRel * rated);
    CHECK_NEAR(out.device.rated_output_mv_v, sensor.rated_mv_v, kRatedAccuracyRel * rated);
    // Gespeicherte Kalibrierung verschiebt den simulierten Sensor nicht
    const auto again = ew::SimBackend::sensor_for(out);
    CHECK_NEAR(again.rated_mv_v, sensor.rated_mv_v, kEps);
    CHECK_NEAR(again.zero_mv_v, sensor.zero_mv_v, kEps);

    // Nach dem Abschluss gilt wieder der Scale factor der Config
    const auto ch = ew::Characteristic::from_config(out.calibration, out.device);
    sim.set_sim_load(kCheckRatio * cap);
    ew::Sample s;
    t = run_cycles(sim, cc, cfg, t, kSettleS + kMarginS, &s);
    CHECK_NEAR(*ch.to_grams(s.raw), kCheckRatio * cap, kAccuracyRel * cap);
}

} // namespace

/// @brief Führt alle registrierten Tests aus; Exit-Code 1 bei Fehlern.
int main() {
    for (const auto& tc : registry()) {
        const int before = g_fail;
        tc.fn();
        std::printf("%s %s\n", g_fail == before ? "[ OK ]" : "[FAIL]", tc.name);
    }
    std::printf("%d Checks, %d Fehler\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
