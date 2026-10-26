/**
 * @file
 * @brief Unit tests without an external framework: ew_tests returns exit code != 0 on failures.
 *
 * Numeric values are named constants: taken from the production code (simulation,
 * calibration, object dictionary), from the specification (bit position according to the ESI) or as test data.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "api_registry.hpp"
#include "config.hpp"
#include "device_commands.hpp"
#include "device_description.hpp"
#include "el3356.hpp"
#include "measurement.hpp"
#include "measurement_csv.hpp"
#include "calibration.hpp"
#include "calibration_controller.hpp"
#include "sim_backend.hpp"
#include "units.hpp"
#include "statistics.hpp"

namespace {

int g_fail = 0;    ///< failed checks
int g_checks = 0;  ///< executed checks

/// Checks @p cond and counts failures without aborting the test.
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

/// Checks |a - b| <= eps.
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

/// Registered test.
struct TestCase {
    const char* name;          ///< Name
    std::function<void()> fn;  ///< test function
};
/// @brief List of all registered tests.
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
/// Registers a test during static initialization.
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
/// Defines and registers a test.
#define TEST(name)                         \
    void name();                           \
    Reg reg_##name(#name, name);           \
    void name()

// --- Common constants ------------------------------------------------------------

constexpr double kMsPerS = 1000.0;  ///< milliseconds per second

/// Cycle time of the default configuration [s]
const double kCycleS = ew::Config{}.ethercat.cycle_ms / kMsPerS;

constexpr double kEps = 1e-12;       ///< tolerance for exactly computable values
constexpr double kEpsCoarse = 1e-9;  ///< tolerance for rounding over several computation steps
constexpr double kEpsFloat = 1e-6;   ///< tolerance for REAL32 values
constexpr double kExact = 0.0;       ///< bit-exact comparison

/**
 * @brief Number of cycles for a duration.
 * @param seconds duration [s]
 * @return rounded number of cycles at kCycleS
 */
int cycles_for(double seconds) {
    return static_cast<int>(std::lround(seconds / kCycleS));
}

/// @brief Shipped configuration (openwrt/ethercat-weigh/files/config.json).
ew::Config package_config() { return ew::load_config(EW_PACKAGE_CONFIG); }

/// @brief Capacity of the simulated sensor in grams.
double capacity_g(const ew::Config& cfg) {
    return ew::SimBackend::sensor_for(cfg).capacity_kg * ew::kGramsPerKg;
}

/// @brief Valid sample with net value @p v [g] at time @p t [s].
ew::Sample valid_sample(double t, double v) {
    ew::Sample s;
    s.t = t;
    s.value = v;
    s.link_ok = true;
    s.wkc_ok = true;
    return s;
}

/// @name Measurement parameters and test loads for the engine tests [g]
/// @{
constexpr double kMinLoad = 0.1;         ///< min_load
constexpr double kZeroThreshold = 0.02;  ///< zero_threshold
constexpr uint32_t kStableTimeMs = 500U; ///< stable_time_ms
constexpr double kTolerance = 0.002;     ///< tolerance
constexpr double kSigmaMax = 0.001;      ///< sigma_max
constexpr double kLoadA = 1.234;         ///< first load
constexpr double kLoadB = 1.0;           ///< load after partial unloading (without re-arming)
constexpr double kLoadC = 2.5;           ///< load after complete unloading
/// Partial unloading: between zero_threshold and min_load, does not arm a new measurement
constexpr double kPartialUnload = (kZeroThreshold + kMinLoad) / 2.0;
/// Noise amplitude: range 2 · amplitude is above tolerance
constexpr double kUnstableAmplitude = 1.5 * kTolerance;
/// @}

/// @name Durations in the engine tests [s]
/// @{
constexpr double kEmptyS = 1.0;          ///< unloaded before the first load
constexpr double kHoldS = 2.0;           ///< hold the load (much longer than stable_time_ms)
constexpr double kUnloadS = 0.2;         ///< partial or complete unloading
constexpr double kUnstableS = 5.0;       ///< unstable load
constexpr double kShortS = 0.3;          ///< shorter than stable_time_ms
constexpr double kSteadyBitS = 1.0;      ///< load with the steady state bit set
/// @}

/// @brief Measurement parameters for the engine tests.
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
    // Test series 1..5: mean 3, variance (n-1) 2.5
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
    const std::vector<double> held = {10.0, 12.0, 11.0, 13.0};  // all without kOutlier
    constexpr double kHeldMean = 11.5;
    constexpr double kHeldMax = 13.0;
    constexpr double kWindowMean = 12.0;  // last kWindow values: 12, 11, 13

    // All values held: result identical to the recomputation without the value
    ew::TemporaryStatistics st(kWindow);
    for (double x : all) st.add(x);
    st.remove(kOutlier, held);
    auto j = st.to_json();
    CHECK(j["session"]["n"] == held.size());
    CHECK_NEAR(j["session"]["mean"].get<double>(), kHeldMean, kEps);
    CHECK_NEAR(j["session"]["max"].get<double>(), kHeldMax, kExact);
    CHECK(j["rolling"]["n"] == kWindow);
    CHECK_NEAR(j["rolling"]["mean"].get<double>(), kWindowMean, kEps);

    // Only a part held: n/mean/standard deviation exact via Welford backwards
    constexpr double kRemoved = 11.0;
    const std::vector<double> ref_values = {10.0, 12.0, 30.0, 13.0};  // all without kRemoved
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
    // Partial unloading above zero_threshold: no re-arming
    for (int i = 0; i < cycles_for(kUnloadS); ++i, t += kCycleS) e.process(valid_sample(t, kPartialUnload));
    for (int i = 0; i < cycles_for(kHoldS); ++i, t += kCycleS)
        if (e.process(valid_sample(t, kLoadB))) ++captures;
    CHECK(captures == 1);
    // Unloading below zero_threshold: re-armed
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
    constexpr double kNegativeRated = -1.5;   // negative sign allowed
    constexpr double kJsonMinLoad = 300.0;
    constexpr double kChangedTolerance = 0.01;
    constexpr double kNominalFactor = 2.0;

    ew::Config c;
    CHECK(c.validate().empty());
    c.measurement.zero_threshold = c.measurement.min_load * kNominalFactor;  // zero threshold > minimum load
    CHECK(!c.validate().empty());

    ew::Config cr;
    cr.device.rated_output_mv_v = kNegativeRated;
    CHECK(cr.validate().empty());
    cr.device.rated_output_mv_v = 0.0;
    CHECK(!cr.validate().empty());

    ew::Config cu;
    cu.ui.hidden_columns = {std::string(ew::kMeasurementColumns.front()),
                            std::string(ew::kMeasurementColumns.back())};
    CHECK(cu.validate().empty());
    cu.ui.hidden_columns.emplace_back("unknown");
    CHECK(!cu.validate().empty());

    ew::Config cd;
    cd.ui.display_average = 1U;  // without smoothing
    CHECK(cd.validate().empty());
    cd.ui.display_average = 0U;
    CHECK(!cd.validate().empty());

    const nlohmann::json j = {{"measurement", {{"min_load", kJsonMinLoad}}}, {"unknown", 1}};
    const auto c2 = ew::config_from_json(j);
    CHECK_NEAR(c2.measurement.min_load, kJsonMinLoad, kEps);
    CHECK(c2.ethercat.cycle_ms == ew::Config{}.ethercat.cycle_ms);  // default is kept
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

/// @name Bit position in RMB Status 0x1A00 and RMB Value (Real) 0x1A02 according to the ESI
/// @{
constexpr uint32_t kBitOverrange = 1U;      ///< 0x6000:02
constexpr uint32_t kBitError = 6U;          ///< 0x6000:07
constexpr uint32_t kBitSteadyState = 8U;    ///< 0x6000:09
constexpr uint32_t kBitSyncError = 13U;     ///< 0x6000:0E or 0x1C32:20
constexpr uint32_t kBitToggle = 15U;        ///< 0x6000:10 or 0x1800:09
constexpr uint32_t kBitValueReal = 16U;     ///< 0x6000:12, directly after 0x1A00 (2 bytes)
constexpr uint32_t kBitTare = 4U;           ///< 0x7000:05 in RMB Control 0x1600
constexpr uint32_t kBitsPerByte = 8U;       ///< bits per byte
/// @}

/**
 * @brief Mapping of RMB Status 0x1A00 according to the ESI; gaps with their width.
 * @param sync_error entry for sync error (revision up to 0x0014: 0x1C32:20)
 * @param toggle entry for TxPDO toggle (revision up to 0x0014: 0x1800:09)
 * @return mapping entries
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

    // Terminal parameters from the device description: the default config writes all 0x8000 entries
    const auto writes = settings_writes(ew::DeviceConfig{});
    CHECK(std::any_of(writes.begin(), writes.end(),
                      [](const SdoWrite& w) { return w.index == kObjSettings && w.sub == kSubRatedOutput; }));
    CHECK(std::none_of(writes.begin(), writes.end(),
                       [](const SdoWrite& w) { return w.index == kObjSettings && w.sub == kSubTara; }));
}

constexpr int kHexBase = 16;  ///< base of the hex texts of the device description

/**
 * @brief Bit field from an init block of the ESI (hex text, little endian).
 * @param hex data
 * @param off bit offset
 * @param len length in bits
 * @return bytes of the field, little endian
 */
std::vector<uint8_t> init_field(const std::string& hex, uint32_t off, uint32_t len) {
    constexpr size_t kHexPerByte = 2U;
    std::vector<uint8_t> bytes;
    for (size_t i = 0U; i + kHexPerByte <= hex.size(); i += kHexPerByte)
        bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, kHexPerByte), nullptr, kHexBase)));
    std::vector<uint8_t> out(ew::el3356::bytes_for_bits(len), 0U);
    for (uint32_t i = 0U; i < len; ++i, ++off)
        if ((bytes.at(off / kBitsPerByte) >> (off % kBitsPerByte)) & 1U)
            out[i / kBitsPerByte] |= static_cast<uint8_t>(1U << (i % kBitsPerByte));
    return out;
}

TEST(device_settings_from_description) {
    using namespace ew::el3356;
    constexpr uint32_t kBitsSubCount = 16U;  ///< init block from subindex 1: SubIndex 000 + gap
    constexpr uint8_t kSubDynChangeTime = 0x13U;
    constexpr double kDynChangeTimeMax = 100.0;  ///< according to the ESI
    const auto& d = ew::DeviceDescription::el3356_0010();

    // Set the typed fields to the ESI defaults: write list = init block of the variant REAL
    nlohmann::json typed = ew::device_json(ew::DeviceConfig{});
    for (const auto& e : d.entries())
        if (e.raw.contains("config_key") && e.raw.contains("default"))
            typed[e.raw.at("config_key").get<std::string>()] = e.raw.at("default");
    const ew::Config cfg = ew::config_from_json({{"device", typed}});
    CHECK(cfg.validate().empty());
    const nlohmann::json* variant = nullptr;
    for (const auto& v : d.json().at("variants"))
        if (v.at("name") == "Standard (REAL)") variant = &v;
    CHECK(variant != nullptr);
    size_t compared = 0U;
    for (const auto& w : settings_writes(cfg.device)) {
        for (const auto& ic : variant ? variant->at("init") : nlohmann::json::array()) {
            if (std::stoul(ic.at("index").get<std::string>(), nullptr, kHexBase) != w.index) continue;
            const auto* e = d.entry(w.index, w.sub);
            const auto field = init_field(ic.at("data").get<std::string>(),
                                          e->raw.at("bit_offset").get<uint32_t>() - kBitsSubCount, e->bits);
            CHECK(decode_value(w.type, field) == decode_value(w.type, w.data));
            ++compared;
        }
    }
    CHECK(compared > 0U);

    // Validation: range, never_write, typed fields, unknown keys
    ew::Config c;
    c.device.coe["0x8000:13"] = kDynChangeTimeMax;
    CHECK(c.validate().empty());
    c.device.coe["0x8000:13"] = kDynChangeTimeMax + 1.0;
    CHECK(c.validate().size() == 1U);
    c.device.coe = {{"0x8000:22", 0.0}};
    CHECK(c.validate().size() == 1U);
    c.device.coe = {{"0x8000:23", 1.0}};
    CHECK(c.validate().size() == 1U);
    c.device.coe = {{"0x8000:99", 1.0}};
    CHECK(c.validate().size() == 1U);
    c.device.coe = {{"8000:13", 1.0}};
    CHECK(c.validate().size() == 1U);
    c.device.coe = {};
    c.device.mode0_filter_settings = kFilterPdoFrequency + 1U;
    CHECK(c.validate().size() == 1U);
    const auto* dyn = d.entry(kObjSettings, kSubDynChangeTime);
    CHECK(dyn && dyn->raw.at("max") == kDynChangeTimeMax);
}

TEST(el3356_debug_catalog_and_init_sequence) {
    using namespace ew::el3356;
    constexpr float kValueReal = -12.25F;
    constexpr double kLargeU64 = 1e12;
    constexpr int16_t kNegativeI16 = -2;
    constexpr uint8_t kSubCtrlInvalid = kSubCtrlTare + 1U;  // first unused subindex
    constexpr size_t kAssignFrame = 2U;  // per assignment: clear count + set count

    const auto assign = pdo_assignment(ew::DeviceConfig{});
    auto pdos = known_pdos();
    locate_pdos(pdos, assign.rx, assign.tx);
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

    // Write status bits + Value (Real) and read them back via layout and catalog
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

    // Up to revision 0x0014 (ESI): sync error 0x1C32:20, TxPDO toggle 0x1800:09
    const auto old = layout_inputs({status_mapping({kObjSmOutParam, kSubSmOutSyncError, kBitsBool},
                                                   {kObjTxPdoPar, kSubTxPdoParToggle, kBitsBool})});
    CHECK(old.sync_error.present && old.sync_error.bit_offset == kBitSyncError);
    CHECK(old.toggle.present && old.toggle.bit_offset == kBitToggle);

    // Type encoding
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
    const size_t rx_len = assign.rx.size() + kAssignFrame;
    const size_t tx_len = assign.tx.size() + kAssignFrame;
    const auto seq = init_sequence(dc);
    CHECK(seq.size() == rx_len + tx_len + settings_writes(dc).size());
    CHECK(seq.front().index == kSm2Assign && seq.front().sub == kSubCount && seq.front().pre_op_only);
    const auto pos = std::find(assign.tx.begin(), assign.tx.end(), kPdoValueReal) - assign.tx.begin();
    const auto real_assign = std::find_if(seq.begin(), seq.end(), [&](const SdoWrite& w) {
        return w.index == kSm3Assign && w.sub == kSubFirstEntry + pos;
    });
    CHECK(real_assign != seq.end() && decode_value(DType::U16, real_assign->data) == kPdoValueReal);
    dc.write_on_start = false;
    CHECK(init_sequence(dc).size() == rx_len + tx_len);
}

TEST(device_description_el3356) {
    using namespace ew::el3356;
    constexpr uint32_t kRevNewest = 0x0019000AU;     ///< latest revision of the ESI
    constexpr uint32_t kRevBefore1601 = 0x0014000AU; ///< last revision without 0x1601
    constexpr uint8_t kSubDynChangeTime = 0x13U;     ///< 0x8000:13 Dynamic filter change time
    constexpr double kDynChangeTimeFactor = 0.01;    ///< ConvertValue: 1/100 s
    constexpr uint16_t kFilterDynamicIir = 10U;      ///< DT0800EN16: Dynamic IIR

    const auto& d = ew::DeviceDescription::el3356_0010();
    CHECK(d.json().at("device").at("type") == "EL3356-0010" && d.newest_revision() == kRevNewest);
    CHECK(d.json().at("warnings").is_array());

    const auto* dyn = d.entry(kObjSettings, kSubDynChangeTime);
    CHECK(dyn != nullptr && dyn->type == "u16");
    if (dyn) {
        CHECK_NEAR(dyn->raw.at("convert").at("factor").get<double>(), kDynChangeTimeFactor, kEps);
        const auto& en = dyn->raw.at("enable_if");
        CHECK(en.size() == 2U && en.at(0).at("obj") == "0x8000:11" && en.at(0).at("eq") == kFilterDynamicIir);
    }
    const auto* rated = d.entry(kObjSettings, kSubRatedOutput);
    CHECK(rated && rated->raw.at("config_key") == "rated_output_mv_v");

    // 0x1601 only from revision 0x0015; mapping of 0x1A00 up to 0x0014 with 0x1C32:20 and 0x1800:09
    const auto* ff = d.pdo(kPdoFilterFrequency);
    CHECK(ff && d.layout(*ff) != nullptr && d.layout(*ff, kRevBefore1601) == nullptr);
    const auto* status = d.pdo(kPdoStatus);
    const auto* old = status ? d.layout(*status, kRevBefore1601) : nullptr;
    CHECK(old && std::any_of(old->entries.begin(), old->entries.end(), [](const ew::DescPdoEntry& e) {
              return e.index == kObjTxPdoPar && e.sub == kSubTxPdoParToggle;
          }));

    // Catalog from the description: names and types as in the object dictionary
    std::string name;
    DType type = DType::None;
    CHECK(known_entry(kObjTxPdoPar, kSubTxPdoParToggle, name, type) && name == "TxPDO Toggle" &&
          type == DType::Bool);
    CHECK(known_entry(kObjInputs, kSubInValueReal, name, type) && name == "Value (Real)" && type == DType::F32);
    CHECK(known_pdo_name(kPdoValueReal) == "RMB Value (Real)");
    for (const auto& p : known_pdos()) {
        const auto* dp = d.pdo(p.index);
        CHECK(dp && p.excludes == dp->exclude && p.rx == dp->rx);
    }
}

TEST(pdo_assignment_variants) {
    using namespace ew::el3356;
    constexpr uint32_t kRevBefore1601 = 0x0014000AU;  ///< last revision without 0x1601
    constexpr uint16_t kFrequency = 500U;             ///< 50 Hz in 0.1 Hz
    constexpr uint16_t kFrequencyTooHigh = 2001U;     ///< above the value range 1..2000
    constexpr int32_t kValueInt = -1234;

    ew::DeviceConfig dc;
    auto a = pdo_assignment(dc);
    CHECK(a.rx == std::vector<uint16_t>{kPdoControl});
    CHECK((a.tx == std::vector<uint16_t>{kPdoStatus, kPdoValueReal}));
    dc.pdo_variant = kVariantInt32;
    a = pdo_assignment(dc);
    CHECK((a.tx == std::vector<uint16_t>{kPdoStatus, kPdoValueInt}));
    CHECK(check_assignment(a).empty());
    CHECK(!check_assignment({{kPdoControl}, {kPdoStatus, kPdoValueInt, kPdoValueReal}}).empty());

    // Filter 11 (PDO Filter frequency): 0x1601 additionally, from revision 0x0015
    dc.mode0_filter_settings = kFilterPdoFrequency;
    a = pdo_assignment(dc);
    CHECK((a.rx == std::vector<uint16_t>{kPdoControl, kPdoFilterFrequency}));
    CHECK(pdo_assignment(dc, kRevBefore1601).rx == std::vector<uint16_t>{kPdoControl});
    ew::Config c;
    c.device = dc;
    CHECK(!c.validate().empty());  // filter_frequency 0 outside 1..2000
    c.device.filter_frequency = kFrequency;
    CHECK(c.validate().empty());
    c.device.filter_frequency = kFrequencyTooHigh;
    CHECK(!c.validate().empty());
    c.device.filter_frequency = kFrequency;
    c.device.pdo_variant = "x";
    CHECK(!c.validate().empty());

    // Layout: INT32 value and filter frequency in the process image
    ew::Config a_cfg, b_cfg;
    b_cfg.device = dc;
    CHECK(ew::pdo_config_changed(a_cfg, b_cfg) && !ew::device_config_changed(b_cfg, [&] {
        auto x = b_cfg;
        x.device.sample_mode = !x.device.sample_mode;
        return x;
    }()));
    auto pdos = known_pdos();
    locate_pdos(pdos, a.rx, a.tx);
    std::vector<std::vector<MapEntry>> rx, tx;
    for (uint16_t idx : a.rx)
        for (const auto& p : pdos)
            if (p.index == idx) {
                rx.emplace_back();
                for (const auto& e : p.entries) rx.back().push_back(e.map);
            }
    for (uint16_t idx : a.tx)
        for (const auto& p : pdos)
            if (p.index == idx) {
                tx.emplace_back();
                for (const auto& e : p.entries) tx.back().push_back(e.map);
            }
    const auto il = layout_inputs(tx);
    CHECK(il.value_int.present && !il.value_real.present);
    std::vector<uint8_t> img(bytes_for_bits(il.value_int.bit_offset + kBitsDint), 0U);
    encode_inputs(img.data(), il, ew::El3356Status{}, static_cast<float>(kValueInt));
    CHECK(decode_measurement(img.data(), il) == kValueInt);
    const auto ol = layout_outputs(rx);
    CHECK(ol.filter_frequency.present && ol.filter_frequency.bit_len == kBitsUint);
    std::vector<uint8_t> out(bytes_for_bits(ol.filter_frequency.bit_offset + kBitsUint), 0U);
    ew::El3356Control ctrl;
    ctrl.filter_frequency = kFrequency;
    encode_control(out.data(), ol, ctrl);
    uint16_t back = 0U;
    std::memcpy(&back, out.data() + ol.filter_frequency.bit_offset / kBitsPerByte, sizeof back);
    CHECK(back == kFrequency);
}

TEST(device_commands_and_diagnostics) {
    constexpr double kStepS = 0.01;      ///< time step of the status polls [s]
    constexpr int kMaxSteps = 100;       ///< > SimBackend::kCmdBusyReads
    constexpr double kTareTolRel = 1e-3; ///< residual value after tare (fraction of nominal load · scale factor)
    static_assert(kMaxSteps > ew::SimBackend::kCmdBusyReads, "Statusabfragen bis zum Abschluss");

    ew::SimBackend sim;
    ew::Config cfg;
    std::string err;
    CHECK(sim.open(cfg, err));
    ew::Sample s;
    sim.cycle(0.0, ew::El3356Control{}, s);  // load profile starts unloaded: tare to the zero point

    ew::DeviceCommandRunner r;
    bool finished = false;
    CHECK(!r.start("unbekannt", sim, 0.0, finished).ok);
    CHECK(r.start("tare_temporary", sim, 0.0, finished).ok && !finished && r.active());
    CHECK(!r.start("tare_temporary", sim, 0.0, finished).ok);  // still running
    std::optional<ew::CalResult> done;
    double t = 0.0;
    for (int i = 0; i < kMaxSteps && !done; ++i) done = r.step(sim, t += kStepS);
    CHECK(done && done->ok && !r.active());
    sim.set_sim_load(0.0);
    sim.cycle(t, ew::El3356Control{}, s);
    const double tol = kTareTolRel * cfg.device.nominal_load * cfg.device.scale_factor;
    CHECK(std::fabs(s.raw) < tol);

    // Tare reset (ESI, CmdObjectButton) writes 0x8000:22 without status polling
    std::string reset_id;
    for (const auto& c : ew::DeviceDescription::el3356_0010().json().at("commands"))
        if (c.at("writes").size() == 1U && c.at("writes").at(0).at("obj") == "0x8000:22") reset_id = c.at("id");
    CHECK(!reset_id.empty() && r.start(reset_id, sim, t, finished).ok && finished && !r.active());

    const auto diag = ew::read_diagnostics(sim);
    CHECK(diag.at("ok") == true && diag.at("items").size() > 1U);
    CHECK(diag.at("items").at(0).at("key") == "0x9000:11" && diag.at("items").at(0).at("value").is_number());
}

/// @name Simulation
/// @{
constexpr unsigned kSeedEndToEnd = 42U;    ///< random seed: load profile
constexpr unsigned kSeedAppCal = 7U;       ///< random seed: calibration in the application
constexpr unsigned kSeedAbort = 3U;        ///< random seed: abort by overrange
constexpr unsigned kSeedDeviceCal = 9U;    ///< random seed: calibration in the terminal
constexpr int kEndToEndPeriods = 4;        ///< periods of the load profile in the end-to-end test
constexpr double kPeriodMargin = 0.3;      ///< additional run time (fraction of a period)
constexpr double kLoadMargin = 0.01;       ///< tolerance of the captured load (fraction of capacity)
/// Settling of the simulation: 10 time constants (residual oscillation < 5e-5)
constexpr double kSettleS = 10.0 * ew::SimBackend::kSettleTauS;
constexpr double kMarginS = 0.5;           ///< safety margin on waiting times [s]
constexpr double kCmdWaitS = 1.0;          ///< > SimBackend::kCmdBusyReads status polls
constexpr double kAccuracyRel = 1e-3;      ///< accuracy after calibration (fraction of capacity)
constexpr double kZeroAccuracyRel = 1e-4;  ///< accuracy of zero balance (fraction of the rated value)
constexpr double kRatedAccuracyRel = 2e-3; ///< accuracy of rated output (fraction of the rated value)
/// @}

/// @name Test loads as a fraction of capacity
/// @{
constexpr double kRefRatio = 2.0 * ew::CalibrationController::kMinReferenceRatio;  ///< permitted reference
constexpr double kSmallRefRatio = ew::CalibrationController::kMinReferenceRatio / 2.0;  ///< reference too small
constexpr double kCheckRatio = 0.6;      ///< check load after the calibration in the terminal
constexpr double kAppCheckRatio = 0.2;   ///< check load after the calibration in the application
constexpr double kOverloadRatio = 1.5;   ///< load in overrange
static_assert(kOverloadRatio > ew::SimBackend::kOverrangeFactor, "Last muss im Overrange liegen");
static_assert(kCheckRatio < 1.0 && kAppCheckRatio < 1.0, "Prüflasten innerhalb der Nennlast");
/// @}

TEST(simulation_end_to_end) {
    ew::Config cfg = package_config();
    const auto sensor = ew::SimBackend::sensor_for(cfg);
    const double cap = capacity_g(cfg);
    cfg.device.zero_balance = sensor.zero_mv_v;  // assume the terminal is calibrated
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
    // Independent reference values: 1 gr = 64.79891 mg (definition), 1 kg = 1000 g, 1 g = 1000 mg
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

TEST(measurement_csv_roundtrip_and_formats) {
    constexpr double kValueG = 25.125;
    constexpr double kStddevG = 0.0125;
    constexpr double kRangeG = 0.05;
    constexpr uint32_t kSamples = 50U;
    constexpr double kSettleMs = 1210.0;
    constexpr double kTimeS = 12.5;
    constexpr double kPlainG = 12.5;   // single column with decimal comma
    constexpr double kCommaG = 7.25;   // comma as separator

    // Export -> import in milligrams: same values in grams
    ew::Measurement m;
    m.id = 1U;
    m.t = kTimeS;
    m.value = kValueG;
    m.stddev = kStddevG;
    m.range = kRangeG;
    m.n = kSamples;  // is exported, ignored on import
    m.settle_ms = kSettleMs;
    std::istringstream exp(ew::measurements_csv_header("mg") + ew::measurement_csv_row(m, "mg") + "\r\n");
    const auto r = ew::parse_measurements_csv(exp);
    CHECK(r.size() == 1U);
    if (r.size() == 1U) {
        CHECK_NEAR(r[0].value, kValueG, kEps);
        CHECK_NEAR(r[0].stddev, kStddevG, kEps);
        CHECK_NEAR(r[0].range, kRangeG, kEps);
        CHECK_NEAR(r[0].settle_ms, kSettleMs, kEps);
        CHECK_NEAR(r[0].t, kTimeS, kEps);
        CHECK(r[0].n == 0U);  // column n is ignored on import
    }
    CHECK(ew::measurements_csv_header("g").find(std::string(";") + ew::kCsvColSamples + ";") != std::string::npos);

    std::istringstream plain("12,5\n\n" + std::to_string(kCommaG) + "\n");
    const auto p = ew::parse_measurements_csv(plain);
    CHECK(p.size() == 2U);
    if (p.size() == 2U) {
        CHECK_NEAR(p[0].value, kPlainG, kEps);
        CHECK_NEAR(p[1].value, kCommaG, kEps);
    }

    std::istringstream comma("t_s,value_g\n1.5," + std::to_string(kCommaG) + "\n");
    const auto c = ew::parse_measurements_csv(comma);
    CHECK(c.size() == 1U && std::fabs(c[0].value - kCommaG) <= kEps);

    const auto fails = [](const std::string& text) {
        std::istringstream is(text);
        try {
            (void)ew::parse_measurements_csv(is);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(fails("id;t_s\n1;2\n"));         // value column missing
    CHECK(fails("value_lb\n1\n"));          // unknown unit
    CHECK(fails("value_g\nabc\n"));         // invalid number
}

TEST(characteristic_device) {
    constexpr double kScaleKg = 1000.0;  // Value = load[kg] · 1000 -> Value corresponds to grams
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
    // Calibration points (raw, g), unsorted; slope 0.5 g up to raw 1000, then 0.55 g
    const std::vector<ew::CalPoint> pts = {{1000.0, 500.0}, {0.0, 0.0}, {3000.0, 1600.0}};
    struct Expect {
        double raw;  ///< raw value
        double g;    ///< expected mass
    };
    const std::vector<Expect> expect = {
        {0.0, 0.0}, {500.0, 250.0}, {2000.0, 1050.0},
        {-100.0, -50.0},   // extrapolation first segment
        {4000.0, 2150.0},  // extrapolation last segment
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
    // Calibration points (raw, g) as test data
    CHECK(!ew::validate_points({{0, 0}}).empty());                     // only one point
    CHECK(!ew::validate_points({{0, 0}, {0, 100}}).empty());           // same raw value
    CHECK(!ew::validate_points({{0, 0}, {1, 100}, {2, 50}}).empty());  // not monotonic
    CHECK(ew::validate_points({{0, 0}, {-1, 100}}).empty());           // falling characteristic allowed
    CHECK(ew::validate_points({{0, 0}, {1, 100}, {3, 300}}).empty());
}

/**
 * @brief Runs cycles (kCycleS) with simulation and calibration control.
 * @param sim simulation
 * @param cc calibration control
 * @param cfg configuration
 * @param t start time [s]
 * @param seconds duration [s]
 * @param[out] last last sample (optional)
 * @return end time [s]
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

/// @brief Waiting time for a calibration point: capture time of the config plus margin [s].
double capture_wait_s(const ew::Config& cfg) {
    return cfg.calibration.capture_time_ms / kMsPerS + kMarginS;
}

TEST(calibration_application_flow) {
    constexpr double kRefLoadRatio = 0.4;  // second calibration point
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
    const double m2_gr = ew::from_grams(m2, "gr");  // specify the mass in grains
    sim.set_sim_load(m2);
    t = run_cycles(sim, cc, cfg, t, kSettleS);
    CHECK(cc.capture(ew::to_grams(m2_gr, "gr"), t).ok);
    t = run_cycles(sim, cc, cfg, t, capture_wait_s(cfg));
    CHECK(cc.points().size() == 2U);
    CHECK(cc.remove_point(cc.points().size()).ok == false);  // index after the last point
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
    const ew::Config cfg = package_config();  // terminal with nominal values of the config (uncalibrated)
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
    CHECK(!cc.device_zero(sim, t).ok);       // not yet stable for stable_time_s
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
    // Result matches the simulated sensor (not the nominal values of the config)
    const double rated = std::fabs(sensor.rated_mv_v);
    CHECK(std::fabs(sensor.rated_mv_v - cfg.device.rated_output_mv_v) > ew::SimBackend::kRatedDev / 2.0 * rated);
    CHECK_NEAR(out.device.zero_balance, sensor.zero_mv_v, kZeroAccuracyRel * rated);
    CHECK_NEAR(out.device.rated_output_mv_v, sensor.rated_mv_v, kRatedAccuracyRel * rated);
    // A saved calibration does not shift the simulated sensor
    const auto again = ew::SimBackend::sensor_for(out);
    CHECK_NEAR(again.rated_mv_v, sensor.rated_mv_v, kEps);
    CHECK_NEAR(again.zero_mv_v, sensor.zero_mv_v, kEps);

    // After completion, the scale factor of the config applies again
    const auto ch = ew::Characteristic::from_config(out.calibration, out.device);
    sim.set_sim_load(kCheckRatio * cap);
    ew::Sample s;
    t = run_cycles(sim, cc, cfg, t, kSettleS + kMarginS, &s);
    CHECK_NEAR(*ch.to_grams(s.raw), kCheckRatio * cap, kAccuracyRel * cap);
}

TEST(api_registry_openapi) {
    using crow::HTTPMethod;
    const auto ok = [] { return crow::response(crow::status::OK); };
    crow::SimpleApp app;
    ew::ApiRegistry api(app);
    ew::ApiDoc del{"t", "Delete", nullptr, {{crow::status::OK, nullptr}}};
    del.params = {"id"};
    api.add(HTTPMethod::Delete, "/x/<uint>", del, [](uint64_t) { return crow::response(crow::status::OK); });
    api.add(HTTPMethod::Get, "/y", {"t", "Read", nullptr, {{crow::status::OK, {{"type", "object"}}}}}, ok);
    api.add(HTTPMethod::Put, "/y", {"t", "Write", {{"type", "object"}}, {{crow::status::OK, nullptr}}},
            [](const crow::request&) { return crow::response(crow::status::OK); });
    const auto d = api.openapi("1.0", "Test");
    CHECK(d.at("openapi") == "3.1.0");
    CHECK(d["paths"].contains("/x/{id}"));
    CHECK(d["paths"]["/x/{id}"]["delete"]["parameters"][0]["name"] == "id");
    CHECK(d["paths"]["/x/{id}"]["delete"]["parameters"][0]["schema"]["type"] == "integer");
    CHECK(d["paths"]["/y"].contains("get") && d["paths"]["/y"].contains("put"));
    CHECK(!d["paths"]["/y"]["get"].contains("requestBody"));
    CHECK(d["paths"]["/y"]["put"].contains("requestBody"));
    CHECK(d["paths"]["/y"]["get"]["responses"]["200"]["content"].contains("application/json"));

    // Missing parameter name, duplicate route, mismatching handler
    auto throws = [](const std::function<void()>& f) {
        try {
            f();
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(throws([&] { api.add(HTTPMethod::Get, "/z/<uint>", {"t", "", nullptr, {}}, ok); }));
    CHECK(throws([&] { api.add(HTTPMethod::Get, "/y", {"t", "", nullptr, {}}, ok); }));
    ew::ApiDoc mismatch{"t", "", nullptr, {}};
    mismatch.params = {"id"};
    CHECK(throws([&] { api.add(HTTPMethod::Get, "/w/<uint>", mismatch, ok); }));

    const auto s = ew::schema_from({{"a", 1},
                                    {"b", 1.5},
                                    {"c", {{"d", true}}},
                                    {"e", nlohmann::json::array({"x"})},
                                    {"f", nullptr}});
    CHECK(s["type"] == "object" && !s.contains("required"));
    CHECK(s["properties"]["a"]["type"] == "integer");
    CHECK(s["properties"]["b"]["type"] == "number");
    CHECK(s["properties"]["c"]["properties"]["d"]["type"] == "boolean");
    CHECK(s["properties"]["e"]["items"]["type"] == "string");
    CHECK(s["properties"]["f"].empty());
}

TEST(openapi_file_consistent) {
    std::ifstream f(EW_OPENAPI_FILE);
    CHECK(f.good());
    if (!f) return;
    const auto d = nlohmann::json::parse(f);
    CHECK(!d.at("paths").empty());
    // Every operation has a summary and responses
    for (const auto& [path, item] : d.at("paths").items())
        for (const auto& [verb, op] : item.items()) {
            CHECK(!op.value("summary", std::string()).empty());
            CHECK(!op.value("responses", nlohmann::json::object()).empty());
        }
    // All $ref can be resolved
    std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& j) {
        if (j.is_object()) {
            if (j.contains("$ref")) {
                const std::string ref = j.at("$ref");
                CHECK(ref.rfind("#/", 0) == 0 && d.contains(nlohmann::json::json_pointer(ref.substr(1))));
            }
            for (const auto& v : j) walk(v);
        } else if (j.is_array()) {
            for (const auto& v : j) walk(v);
        }
    };
    walk(d);
}

} // namespace

/// @brief Runs all registered tests; exit code 1 on failures.
int main() {
    for (const auto& tc : registry()) {
        const int before = g_fail;
        tc.fn();
        std::printf("%s %s\n", g_fail == before ? "[ OK ]" : "[FAIL]", tc.name);
    }
    std::printf("%d Checks, %d Fehler\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
