// Unit-Tests ohne externes Framework: ew_tests liefert Exit-Code != 0 bei Fehlern.
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

int g_fail = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name)                         \
    void name();                           \
    Reg reg_##name(#name, name);           \
    void name()

ew::Sample valid_sample(double t, double v) {
    ew::Sample s;
    s.t = t;
    s.value = v;
    s.link_ok = true;
    s.wkc_ok = true;
    return s;
}

ew::MeasurementConfig mparams() {
    ew::MeasurementConfig p;
    p.min_load = 0.1;
    p.zero_threshold = 0.02;
    p.stable_time_ms = 500;
    p.tolerance = 0.002;
    p.sigma_max = 0.001;
    return p;
}

// ---------------------------------------------------------------------------
TEST(running_stats_basic) {
    ew::RunningStats r;
    for (double x : {1.0, 2.0, 3.0, 4.0, 5.0}) r.add(x);
    const auto s = r.summary();
    CHECK(s.n == 5);
    CHECK_NEAR(s.mean, 3.0, 1e-12);
    CHECK_NEAR(s.stddev, std::sqrt(2.5), 1e-12);
    CHECK_NEAR(s.min, 1.0, 0);
    CHECK_NEAR(s.max, 5.0, 0);
    CHECK_NEAR(s.range, 4.0, 0);
    CHECK_NEAR(s.last, 5.0, 0);
    r.reset();
    CHECK(r.summary().n == 0);
}

TEST(rolling_stats_window) {
    ew::RollingStats r(3);
    for (double x : {10.0, 1.0, 2.0, 3.0}) r.add(x);
    const auto s = r.summary();
    CHECK(s.n == 3);
    CHECK_NEAR(s.mean, 2.0, 1e-12);
    CHECK_NEAR(s.max, 3.0, 0);
    r.resize(2);
    CHECK(r.summary().n == 2);
    CHECK_NEAR(r.summary().mean, 2.5, 1e-12);
}

TEST(measurement_single_capture_and_rearm) {
    ew::MeasurementEngine e(mparams());
    const double dt = 0.01;
    double t = 0.0;
    int captures = 0;
    double captured = 0.0;
    // 1 s leer
    for (int i = 0; i < 100; ++i, t += dt) CHECK(!e.process(valid_sample(t, 0.0)));
    CHECK(e.state() == ew::MeasState::Idle);
    // 2 s Last 1.234
    for (int i = 0; i < 200; ++i, t += dt) {
        if (auto m = e.process(valid_sample(t, 1.234))) {
            ++captures;
            captured = m->value;
            CHECK(m->n >= 50);
            CHECK(m->settle_ms >= 500.0 - 1e-6);
        }
    }
    CHECK(captures == 1);
    CHECK_NEAR(captured, 1.234, 1e-12);
    CHECK(e.state() == ew::MeasState::WaitUnload);
    // Teilentlastung über zero_threshold: kein Re-Arm
    for (int i = 0; i < 20; ++i, t += dt) e.process(valid_sample(t, 0.05));
    for (int i = 0; i < 200; ++i, t += dt)
        if (e.process(valid_sample(t, 1.0))) ++captures;
    CHECK(captures == 1);
    // Entlastung unter zero_threshold: Re-Arm
    for (int i = 0; i < 20; ++i, t += dt) e.process(valid_sample(t, 0.0));
    CHECK(e.armed());
    for (int i = 0; i < 200; ++i, t += dt)
        if (auto m = e.process(valid_sample(t, 2.5))) {
            ++captures;
            CHECK_NEAR(m->value, 2.5, 1e-12);
        }
    CHECK(captures == 2);
}

TEST(measurement_unstable_no_capture) {
    ew::MeasurementEngine e(mparams());
    double t = 0.0;
    int captures = 0;
    for (int i = 0; i < 500; ++i, t += 0.01) {
        const double v = 1.0 + ((i % 2) ? 0.003 : -0.003);  // Spannweite 0.006 > tolerance
        if (e.process(valid_sample(t, v))) ++captures;
    }
    CHECK(captures == 0);
    CHECK(e.state() == ew::MeasState::Settling);
    CHECK(e.window().range > 0.002);
}

TEST(measurement_fault_clears_window) {
    ew::MeasurementEngine e(mparams());
    double t = 0.0;
    for (int i = 0; i < 30; ++i, t += 0.01) e.process(valid_sample(t, 1.0));
    CHECK(e.window().n == 30);
    auto s = valid_sample(t, 1.0);
    s.status.calibration_in_progress = true;
    CHECK(!e.process(s));
    CHECK(e.state() == ew::MeasState::Fault);
    CHECK(e.fault_reason() == "calibration");
    CHECK(e.window().n == 0);
    auto w = valid_sample(t, 1.0);
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
    for (int i = 0; i < 200; ++i, t += 0.01)
        if (e.process(valid_sample(t, 1.0))) ++captures;
    CHECK(captures == 0);
    for (int i = 0; i < 100; ++i, t += 0.01) {
        auto s = valid_sample(t, 1.0);
        s.status.steady_state = true;
        if (e.process(s)) ++captures;
    }
    CHECK(captures == 1);
}

TEST(config_validation_and_json) {
    ew::Config c;
    CHECK(c.validate().empty());
    c.measurement.zero_threshold = 500.0;
    c.measurement.min_load = 100.0;
    CHECK(!c.validate().empty());

    ew::Config cr;
    cr.device.rated_output_mv_v = -1.5;  // negatives Vorzeichen zulässig
    CHECK(cr.validate().empty());
    cr.device.rated_output_mv_v = 0.0;
    CHECK(!cr.validate().empty());

    nlohmann::json j = nlohmann::json::parse(R"({"measurement":{"min_load":300}, "unknown":1})");
    const auto c2 = ew::config_from_json(j);
    CHECK_NEAR(c2.measurement.min_load, 300.0, 1e-12);
    CHECK(c2.ethercat.cycle_ms == 10);  // Default bleibt erhalten
    const auto c3 = ew::config_from_json(nlohmann::json(c2));
    CHECK(nlohmann::json(c3) == nlohmann::json(c2));

    ew::Config a, b;
    b.measurement.tolerance = 0.01;
    CHECK(!ew::requires_restart(a, b));
    b.ethercat.interface = "eth0";
    CHECK(ew::requires_restart(a, b));
    b = a;
    b.device.nominal_load = 10.0;
    CHECK(ew::device_config_changed(a, b));
}

TEST(el3356_layout_and_decode) {
    using ew::el3356::MapEntry;
    // 0x1A00 mit Padding-Einträgen (Index 0x0000), danach 0x1A02 Value (Real)
    std::vector<MapEntry> status = {
        {0x0000, 0, 1}, {0x6000, 0x02, 1}, {0x0000, 0, 1}, {0x6000, 0x04, 1}, {0x0000, 0, 2},
        {0x6000, 0x07, 1}, {0x6000, 0x08, 1}, {0x6000, 0x09, 1}, {0x0000, 0, 4},
        {0x6000, 0x0E, 1}, {0x0000, 0, 1}, {0x6000, 0x10, 1}};
    std::vector<MapEntry> value = {{0x6000, 0x12, 32}};
    const auto l = ew::el3356::layout_inputs({status, value});
    CHECK(l.overrange.present && l.overrange.bit_offset == 1);
    CHECK(l.error.bit_offset == 6);
    CHECK(l.steady_state.bit_offset == 8);
    CHECK(l.toggle.bit_offset == 15);
    CHECK(l.value_real.bit_offset == 16 && l.value_real.bit_len == 32);

    uint8_t img[6] = {};
    img[1] = 0x01;  // Bit 8: Steady state
    const float f = 3.5f;
    std::memcpy(img + 2, &f, 4);
    const auto s = ew::el3356::decode_status(img, l);
    CHECK(s.steady_state && !s.overrange && !s.error);
    CHECK_NEAR(ew::el3356::decode_value_real(img, l), 3.5f, 0);

    const auto ol = ew::el3356::layout_outputs(
        {{{0x7000, 1, 1}, {0x7000, 2, 1}, {0x7000, 3, 1}, {0x7000, 4, 1}, {0x7000, 5, 1}, {0, 0, 11}}});
    uint8_t out[2] = {};
    ew::El3356Control c;
    c.tare = true;
    ew::el3356::encode_control(out, ol, c);
    CHECK(out[0] == 0x10);
    c.tare = false;
    ew::el3356::encode_control(out, ol, c);
    CHECK(out[0] == 0x00);

    const auto raw = (0x6000u << 16) | (0x12u << 8) | 32u;
    const auto me = ew::el3356::decode_map_entry(raw);
    CHECK(me.index == 0x6000 && me.sub == 0x12 && me.bits == 32);
    CHECK(ew::el3356::settings_writes(ew::DeviceConfig{}).size() == 12);
}

TEST(el3356_debug_catalog_and_init_sequence) {
    using namespace ew::el3356;
    auto pdos = known_pdos();
    locate_pdos(pdos, rx_assignment(), tx_assignment());
    const PdoInfo* status = nullptr;
    const PdoInfo* real = nullptr;
    for (const auto& p : pdos) {
        if (p.index == kPdoStatus) status = &p;
        if (p.index == kPdoValueReal) real = &p;
        if (p.index == 0x1A01) CHECK(!p.assigned);
    }
    CHECK(status && status->assigned && real && real->assigned);
    CHECK(real->entries[0].loc.bit_offset == 16 && real->entries[0].type == DType::F32);

    // Statusbits + Value (Real) schreiben und über Layout und Katalog zurücklesen
    std::vector<std::vector<MapEntry>> tx;
    for (const auto* p : {status, real}) {
        std::vector<MapEntry> m;
        for (const auto& e : p->entries) m.push_back(e.map);
        tx.push_back(m);
    }
    const auto il = layout_inputs(tx);
    uint8_t img[6] = {};
    ew::El3356Status st;
    st.steady_state = true;
    st.txpdo_toggle = true;
    encode_inputs(img, il, st, -12.25f);
    const auto back = decode_status(img, il);
    CHECK(back.steady_state && back.txpdo_toggle && !back.error);
    CHECK_NEAR(entry_value(img, real->entries[0]), -12.25, 0);
    CHECK(entry_value(img, status->entries[7]) == 1.0);  // 0x6000:09 Steady state

    // Bis Revision 0x0014 (ESI): Sync error 0x1C32:20, TxPDO Toggle 0x1800:09
    const auto old = layout_inputs({{{0, 0, 1}, {kObjInputs, 0x02, 1}, {0, 0, 1}, {kObjInputs, 0x04, 1},
                                     {0, 0, 2}, {kObjInputs, 0x07, 1}, {kObjInputs, 0x08, 1},
                                     {kObjInputs, 0x09, 1}, {0, 0, 4}, {kObjSmOutParam, 0x20, 1},
                                     {0, 0, 1}, {kObjTxPdoPar, 0x09, 1}}});
    CHECK(old.sync_error.present && old.sync_error.bit_offset == 13);
    CHECK(old.toggle.present && old.toggle.bit_offset == 15);
    CHECK(pdos.size() == 10);

    // Typ-Kodierung
    CHECK(encode_value(DType::U16, 0x1A02) == (std::vector<uint8_t>{0x02, 0x1A}));
    CHECK(decode_value(DType::I16, {0xFE, 0xFF}) == -2.0);
    CHECK_NEAR(decode_value(DType::F32, encode_value(DType::F32, 9.80665)), 9.80665, 1e-6);
    DType t;
    CHECK(dtype_from_name("i32", t) && t == DType::I32 && !dtype_from_name("x", t));
    CHECK(decode_value(DType::U64, encode_value(DType::U64, 1e12)) == 1e12);

    ew::El3356Control c;
    CHECK(control_bit(c, 0x05) == &c.tare && control_bit(c, 0x06) == nullptr);

    ew::DeviceConfig dc;
    const auto seq = init_sequence(dc);
    CHECK(seq.size() == 3 + 4 + 12);
    CHECK(seq[0].index == kSm2Assign && seq[0].sub == 0 && seq[0].pre_op_only);
    CHECK(seq[5].index == kSm3Assign && decode_value(DType::U16, seq[5].data) == kPdoValueReal);
    dc.write_on_start = false;
    CHECK(init_sequence(dc).size() == 7);
}

TEST(simulation_end_to_end) {
    ew::Config cfg;
    cfg.device.zero_balance = ew::SimBackend::kZeroTrue;  // Klemme als kalibriert annehmen
    cfg.device.rated_output_mv_v = ew::SimBackend::kRatedTrue;
    ew::SimBackend sim(42);
    std::string err;
    CHECK(sim.open(cfg, err));
    const auto ch = ew::Characteristic::from_config(cfg.calibration, cfg.device);
    CHECK(ch.valid());
    ew::MeasurementEngine e(cfg.measurement);
    int captures = 0;
    ew::El3356Control ctrl;
    for (int i = 0; i < 3000; ++i) {  // 30 s = ca. 4 Zyklen des Profils
        ew::Sample s;
        sim.cycle(i * 0.01, ctrl, s);
        s.value = s.gross = *ch.to_grams(s.raw);
        if (auto m = e.process(s)) {
            ++captures;
            CHECK(m->value > 400.0 && m->value < 4700.0);
            CHECK(m->range <= cfg.measurement.tolerance);
        }
    }
    CHECK(captures >= 3 && captures <= 5);
}

TEST(units_conversion) {
    CHECK_NEAR(ew::to_grams(1.0, "gr"), 0.06479891, 1e-15);
    CHECK_NEAR(ew::from_grams(64.79891, "gr"), 1000.0, 1e-9);
    CHECK_NEAR(ew::to_grams(2.5, "kg"), 2500.0, 1e-12);
    CHECK_NEAR(ew::from_grams(1.0, "mg"), 1000.0, 1e-9);
    CHECK(!ew::valid_unit("lb"));
}

TEST(characteristic_device) {
    ew::CalibrationConfig cal;
    ew::DeviceConfig dev;
    cal.mode = "device";
    cal.device_unit = "kg";
    dev.scale_factor = 1000.0;  // Value = Last[kg] * 1000
    const auto c = ew::Characteristic::from_config(cal, dev);
    CHECK(c.valid());
    CHECK_NEAR(*c.to_grams(1234.0), 1234.0, 1e-9);
    cal.device_unit = "gr";
    dev.scale_factor = 1.0;
    CHECK_NEAR(*ew::Characteristic::from_config(cal, dev).to_grams(1000.0), 64.79891, 1e-9);
}

TEST(characteristic_application_piecewise) {
    ew::CalibrationConfig cal;
    cal.mode = "application";
    cal.points = {{1000.0, 500.0}, {0.0, 0.0}, {3000.0, 1600.0}};  // unsortiert
    const auto c = ew::Characteristic::from_config(cal, ew::DeviceConfig{});
    CHECK(c.valid());
    CHECK_NEAR(*c.to_grams(0.0), 0.0, 1e-12);
    CHECK_NEAR(*c.to_grams(500.0), 250.0, 1e-12);
    CHECK_NEAR(*c.to_grams(2000.0), 1050.0, 1e-12);
    CHECK_NEAR(*c.to_grams(-100.0), -50.0, 1e-12);   // Extrapolation erstes Segment
    CHECK_NEAR(*c.to_grams(4000.0), 2150.0, 1e-12);  // Extrapolation letztes Segment
    cal.points = {{0.0, 0.0}};
    CHECK(!ew::Characteristic::from_config(cal, ew::DeviceConfig{}).valid());
    CHECK(!ew::Characteristic::from_config(cal, ew::DeviceConfig{}).to_grams(1.0));
}

TEST(validate_points_rules) {
    CHECK(!ew::validate_points({{0, 0}}).empty());
    CHECK(!ew::validate_points({{0, 0}, {0, 100}}).empty());          // gleicher Rohwert
    CHECK(!ew::validate_points({{0, 0}, {1, 100}, {2, 50}}).empty()); // nicht monoton
    CHECK(ew::validate_points({{0, 0}, {-1, 100}}).empty());          // fallende Kennlinie zulässig
    CHECK(ew::validate_points({{0, 0}, {1, 100}, {3, 300}}).empty());
}

// Hilfsfunktion: Zyklen mit Simulation und Kalibriersteuerung
static double run_cycles(ew::SimBackend& sim, ew::CalibrationController& cc, const ew::Config& cfg,
                         double t, double seconds, ew::Sample* last = nullptr) {
    ew::El3356Control ctrl;
    const int n = static_cast<int>(seconds / 0.01);
    for (int i = 0; i < n; ++i, t += 0.01) {
        ew::Sample s;
        sim.cycle(t, ctrl, s);
        cc.step(s, sim, cfg);
        if (last) *last = s;
    }
    return t;
}

TEST(calibration_application_flow) {
    ew::Config cfg;
    ew::SimBackend sim(7);
    std::string err;
    CHECK(sim.open(cfg, err));
    ew::CalibrationController cc;
    CHECK(cc.start_application(cfg).ok);
    double t = 0.0;
    sim.set_sim_load(0.0);
    t = run_cycles(sim, cc, cfg, t, 2.0);
    CHECK(cc.capture(0.0, t).ok);
    t = run_cycles(sim, cc, cfg, t, 2.5);
    CHECK(cc.points().size() == 1);
    sim.set_sim_load(2000.0);
    t = run_cycles(sim, cc, cfg, t, 2.0);
    CHECK(cc.capture(ew::to_grams(30864.7, "gr"), t).ok);  // ca. 2000 g in Grain angegeben
    t = run_cycles(sim, cc, cfg, t, 2.5);
    CHECK(cc.points().size() == 2);
    CHECK(cc.remove_point(5).ok == false);
    ew::Config out;
    CHECK(cc.commit(cfg, out).ok);
    CHECK(!cc.active());
    CHECK(out.calibration.mode == "application");
    CHECK(out.calibration.points.size() == 2);

    const auto ch = ew::Characteristic::from_config(out.calibration, out.device);
    sim.set_sim_load(1000.0);
    ew::Sample s;
    t = run_cycles(sim, cc, cfg, t, 3.0, &s);
    CHECK_NEAR(*ch.to_grams(s.raw), 1000.0 * 2000.0 / ew::to_grams(30864.7, "gr"), 2.0);
}

TEST(calibration_device_flow) {
    ew::Config cfg;  // Klemme unkalibriert: Rated output 2,0, Zero balance 0
    ew::SimBackend sim(9);
    std::string err;
    CHECK(sim.open(cfg, err));
    ew::CalibrationController cc;
    CHECK(cc.start_device(cfg, 5000.0, "g", sim).ok);
    double t = 0.0;
    sim.set_sim_load(0.0);
    t = run_cycles(sim, cc, cfg, t, 2.0);
    CHECK(!cc.device_zero(sim, t).ok);       // noch nicht 10 s stabil
    t = run_cycles(sim, cc, cfg, t, 10.0);
    CHECK(cc.device_zero(sim, t).ok);
    t = run_cycles(sim, cc, cfg, t, 1.0);
    CHECK(cc.phase() == "zero_done");
    sim.set_sim_load(500.0);
    t = run_cycles(sim, cc, cfg, t, 12.0);
    CHECK(!cc.device_reference(500.0, sim, t).ok);  // < 20 % Nennlast
    sim.set_sim_load(2000.0);
    t = run_cycles(sim, cc, cfg, t, 12.0);
    CHECK(cc.device_reference(2000.0, sim, t).ok);
    t = run_cycles(sim, cc, cfg, t, 1.0);
    CHECK(cc.phase() == "done");
    ew::Config out;
    CHECK(cc.commit(cfg, out).ok);
    CHECK(out.calibration.mode == "device" && out.calibration.device_unit == "g");
    CHECK_NEAR(out.device.zero_balance, ew::SimBackend::kZeroTrue, 1e-4);
    CHECK_NEAR(out.device.rated_output_mv_v, ew::SimBackend::kRatedTrue, 2e-3);

    // Nach dem Abschluss gilt wieder der Scale factor der Config (1000)
    const auto ch = ew::Characteristic::from_config(out.calibration, out.device);
    sim.set_sim_load(3000.0);
    ew::Sample s;
    t = run_cycles(sim, cc, cfg, t, 3.0, &s);
    CHECK_NEAR(*ch.to_grams(s.raw), 3000.0, 3.0);
}

} // namespace

int main() {
    for (const auto& tc : registry()) {
        const int before = g_fail;
        tc.fn();
        std::printf("%s %s\n", g_fail == before ? "[ OK ]" : "[FAIL]", tc.name);
    }
    std::printf("%d Checks, %d Fehler\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
