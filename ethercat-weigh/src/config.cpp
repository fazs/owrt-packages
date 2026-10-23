#include "config.hpp"
#include "units.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ew {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EthercatConfig,
    interface, slave_name, cycle_ms, wkc_fault_cycles, rt_priority)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DeviceConfig,
    write_on_start, mode0_enable_filter, mode0_enable_averager, mode0_filter_settings,
    rated_output_mv_v, nominal_load, zero_balance, gravity_of_earth, scale_factor,
    reference_load, steady_state_window, steady_state_tolerance, calibration_interval)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MeasurementConfig,
    min_load, zero_threshold, stable_time_ms, tolerance, sigma_max, use_device_steady_state)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StatisticsConfig, rolling_window, history_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(UiConfig, rate_hz, display_unit)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HttpConfig, bind, port, web_root, threads)

void to_json(nlohmann::json& j, const CalPoint& p) { j = {{"raw", p.raw}, {"mass_g", p.mass_g}}; }
void from_json(const nlohmann::json& j, CalPoint& p) {
    p.raw = j.at("raw").get<double>();
    p.mass_g = j.at("mass_g").get<double>();
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalibrationConfig,
    mode, device_unit, points, capture_time_ms, stable_time_s, stable_band_rel)

void to_json(nlohmann::json& j, const Config& c) {
    j = nlohmann::json{{"ethercat", c.ethercat},       {"device", c.device},
                       {"calibration", c.calibration}, {"measurement", c.measurement},
                       {"statistics", c.statistics},   {"ui", c.ui},
                       {"http", c.http}};
}

void from_json(const nlohmann::json& j, Config& c) {
    const Config d;
    c.ethercat = j.value("ethercat", d.ethercat);
    c.device = j.value("device", d.device);
    c.calibration = j.value("calibration", d.calibration);
    c.measurement = j.value("measurement", d.measurement);
    c.statistics = j.value("statistics", d.statistics);
    c.ui = j.value("ui", d.ui);
    c.http = j.value("http", d.http);
}

Config config_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("Config muss ein JSON-Objekt sein");
    return j.get<Config>();
}

std::vector<std::string> validate_points(const std::vector<CalPoint>& pts) {
    std::vector<std::string> e;
    if (pts.size() < 2) {
        e.push_back("Kennlinie: mindestens 2 Punkte erforderlich");
        return e;
    }
    std::vector<CalPoint> s = pts;
    std::sort(s.begin(), s.end(), [](const CalPoint& a, const CalPoint& b) { return a.raw < b.raw; });
    int dir = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!(s[i].raw > s[i - 1].raw)) {
            e.push_back("Kennlinie: zwei Punkte mit gleichem Rohwert");
            break;
        }
        const double dm = s[i].mass_g - s[i - 1].mass_g;
        const int d = dm > 0 ? 1 : (dm < 0 ? -1 : 0);
        if (d == 0 || (dir != 0 && d != dir)) {
            e.push_back("Kennlinie: Masse nicht streng monoton über dem Rohwert");
            break;
        }
        dir = d;
    }
    return e;
}

std::vector<std::string> Config::validate() const {
    std::vector<std::string> e;
    if (ethercat.interface.empty()) e.push_back("ethercat.interface: leer");
    if (ethercat.cycle_ms < 1 || ethercat.cycle_ms > 1000)
        e.push_back("ethercat.cycle_ms: zulässig 1..1000");
    if (ethercat.wkc_fault_cycles < 1) e.push_back("ethercat.wkc_fault_cycles: >= 1");
    if (ethercat.rt_priority < 0 || ethercat.rt_priority > 99)
        e.push_back("ethercat.rt_priority: zulässig 0..99");

    if (device.nominal_load <= 0.0) e.push_back("device.nominal_load: > 0");
    if (device.reference_load <= 0.0) e.push_back("device.reference_load: > 0");
    if (device.rated_output_mv_v <= 0.0) e.push_back("device.rated_output_mv_v: > 0");
    if (device.gravity_of_earth <= 0.0) e.push_back("device.gravity_of_earth: > 0");
    if (device.scale_factor == 0.0) e.push_back("device.scale_factor: != 0");
    if (device.mode0_filter_settings > 11) e.push_back("device.mode0_filter_settings: zulässig 0..11");

    const auto& cal = calibration;
    if (cal.mode != "device" && cal.mode != "application")
        e.push_back("calibration.mode: 'device' oder 'application'");
    if (!valid_unit(cal.device_unit)) e.push_back("calibration.device_unit: mg, g, kg oder gr");
    if (cal.capture_time_ms < 100 || cal.capture_time_ms > 60000)
        e.push_back("calibration.capture_time_ms: zulässig 100..60000");
    if (cal.stable_time_s < 0.0 || cal.stable_time_s > 600.0)
        e.push_back("calibration.stable_time_s: zulässig 0..600");
    if (cal.stable_band_rel <= 0.0 || cal.stable_band_rel > 0.1)
        e.push_back("calibration.stable_band_rel: zulässig >0..0,1");
    if (cal.mode == "application" && !cal.points.empty())
        for (auto& s : validate_points(cal.points)) e.push_back("calibration.points: " + s);

    const auto& m = measurement;
    if (!(m.min_load > m.zero_threshold))
        e.push_back("measurement.min_load muss > measurement.zero_threshold sein");
    if (m.tolerance <= 0.0) e.push_back("measurement.tolerance: > 0");
    if (m.sigma_max <= 0.0) e.push_back("measurement.sigma_max: > 0");
    if (m.stable_time_ms < 2 * ethercat.cycle_ms)
        e.push_back("measurement.stable_time_ms: >= 2 * ethercat.cycle_ms");

    if (statistics.rolling_window < 2 || statistics.rolling_window > 100000)
        e.push_back("statistics.rolling_window: zulässig 2..100000");
    if (statistics.history_size < 1 || statistics.history_size > 100000)
        e.push_back("statistics.history_size: zulässig 1..100000");
    if (ui.rate_hz < 1 || ui.rate_hz > 100) e.push_back("ui.rate_hz: zulässig 1..100");
    if (!valid_unit(ui.display_unit)) e.push_back("ui.display_unit: mg, g, kg oder gr");
    if (http.port == 0) e.push_back("http.port: != 0");
    if (http.threads < 1 || http.threads > 8) e.push_back("http.threads: zulässig 1..8");
    return e;
}

Config load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Config nicht lesbar: " + path);
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& ex) {
        throw std::runtime_error("Config-JSON ungültig (" + path + "): " + ex.what());
    }
    Config c = config_from_json(j);
    const auto errs = c.validate();
    if (!errs.empty()) {
        std::ostringstream os;
        os << "Config ungültig:";
        for (const auto& s : errs) os << "\n  " << s;
        throw std::runtime_error(os.str());
    }
    return c;
}

void save_config(const std::string& path, const Config& c) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) throw std::runtime_error("Config nicht schreibbar: " + tmp);
        f << nlohmann::json(c).dump(2) << '\n';
        f.flush();
        if (!f) throw std::runtime_error("Schreibfehler: " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        throw std::runtime_error("rename fehlgeschlagen: " + tmp + " -> " + path);
}

bool requires_restart(const Config& a, const Config& b) {
    return nlohmann::json(a.ethercat) != nlohmann::json(b.ethercat) ||
           nlohmann::json(a.http) != nlohmann::json(b.http);
}

bool device_config_changed(const Config& a, const Config& b) {
    return nlohmann::json(a.device) != nlohmann::json(b.device);
}

} // namespace ew
