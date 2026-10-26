#include "config.hpp"
#include "el3356.hpp"
#include "units.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ew {

namespace {

/// @name Permitted value ranges of the configuration
/// @{
constexpr uint32_t kCycleMsMin = 1U;               ///< ethercat.cycle_ms
constexpr uint32_t kCycleMsMax = 1000U;
constexpr uint32_t kWkcFaultCyclesMin = 1U;        ///< ethercat.wkc_fault_cycles
constexpr int kRtPriorityMin = 0;                  ///< ethercat.rt_priority (0 = SCHED_OTHER)
constexpr int kRtPriorityMax = 99;                 ///< highest SCHED_FIFO priority under Linux
constexpr uint32_t kCaptureMsMin = 100U;           ///< calibration.capture_time_ms
constexpr uint32_t kCaptureMsMax = 60000U;
constexpr double kStableTimeSMin = 0.0;            ///< calibration.stable_time_s
constexpr double kStableTimeSMax = 600.0;
constexpr double kStableBandRelMax = 0.1;          ///< calibration.stable_band_rel (> 0)
constexpr uint32_t kMinWindowCycles = 2U;          ///< measurement.stable_time_ms in cycles
constexpr uint32_t kRollingWindowMin = 2U;         ///< statistics.rolling_window
constexpr uint32_t kRollingWindowMax = 100000U;
constexpr uint32_t kHistorySizeMin = 1U;           ///< statistics.history_size
constexpr uint32_t kHistorySizeMax = 100000U;
constexpr uint32_t kRateHzMin = 1U;                ///< ui.rate_hz
constexpr uint32_t kRateHzMax = 100U;
constexpr uint32_t kDisplayAverageMin = 1U;        ///< ui.display_average (1 = without smoothing)
constexpr uint32_t kDisplayAverageMax = 1000U;
constexpr uint16_t kThreadsMin = 1U;               ///< http.threads
constexpr uint16_t kThreadsMax = 8U;
/// @}

constexpr size_t kMinCalPoints = 2U;  ///< calibration points for a characteristic
constexpr int kJsonIndent = 2;        ///< indentation when saving
constexpr size_t kNumLen = 32U;       ///< buffer for a formatted number

/**
 * @brief Number for messages (decimal comma, without superfluous digits).
 * @param v value
 * @return text
 */
std::string num_text(double v) {
    char b[kNumLen];
    std::snprintf(b, sizeof b, "%g", v);
    std::string s = b;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

/**
 * @brief Message for a value range.
 * @param field field name
 * @param lo lower limit
 * @param hi upper limit
 * @return "field: zulässig lo..hi"
 */
std::string range_msg(const char* field, double lo, double hi) {
    return std::string(field) + ": zulässig " + num_text(lo) + ".." + num_text(hi);
}

} // namespace

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EthercatConfig,
    interface, slave_name, cycle_ms, wkc_fault_cycles, rt_priority)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DeviceConfig,
    write_on_start, mode0_enable_filter, mode0_enable_averager, mode0_filter_settings,
    rated_output_mv_v, nominal_load, zero_balance, gravity_of_earth, scale_factor,
    reference_load, steady_state_window, steady_state_tolerance, calibration_interval, coe,
    pdo_variant, sample_mode, disable_calibration, filter_frequency)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MeasurementConfig,
    min_load, zero_threshold, stable_time_ms, tolerance, sigma_max, use_device_steady_state)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StatisticsConfig, rolling_window, history_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(UiConfig, rate_hz, display_average, display_unit, hidden_columns)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HttpConfig, bind, port, web_root, threads)

/// @brief JSON representation of a calibration point ({"raw", "mass_g"}).
void to_json(nlohmann::json& j, const CalPoint& p) { j = {{"raw", p.raw}, {"mass_g", p.mass_g}}; }
/// @brief Reads a calibration point; "raw" and "mass_g" are mandatory.
void from_json(const nlohmann::json& j, CalPoint& p) {
    p.raw = j.at("raw").get<double>();
    p.mass_g = j.at("mass_g").get<double>();
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalibrationConfig,
    mode, device_unit, points, capture_time_ms, stable_time_s, stable_band_rel)

/// @brief JSON representation of the complete configuration.
void to_json(nlohmann::json& j, const Config& c) {
    j = nlohmann::json{{"ethercat", c.ethercat},       {"device", c.device},
                       {"calibration", c.calibration}, {"measurement", c.measurement},
                       {"statistics", c.statistics},   {"ui", c.ui},
                       {"http", c.http}};
}

/// @brief Reads the configuration; missing sections keep the default.
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

/**
 * @brief Configuration from JSON.
 *
 * Unknown keys are ignored, missing ones keep the default.
 *
 * @param j JSON object
 * @return configuration (not validated)
 * @throws std::runtime_error if @p j is not an object or types do not match
 */
Config config_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("Config muss ein JSON-Objekt sein");
    return j.get<Config>();
}

/**
 * @brief JSON representation of the terminal parameters (keys as in the configuration file).
 * @param dc terminal parameters
 * @return JSON object
 */
nlohmann::json device_json(const DeviceConfig& dc) {
    return dc;
}

/**
 * @brief Checks the calibration points of the characteristic.
 *
 * Required: at least 2 points, raw pairwise different, mass strictly monotonic over raw.
 *
 * @param pts calibration points (any order)
 * @return error messages; empty = valid
 */
std::vector<std::string> validate_points(const std::vector<CalPoint>& pts) {
    std::vector<std::string> e;
    if (pts.size() < kMinCalPoints) {
        e.push_back("Kennlinie: mindestens " + std::to_string(kMinCalPoints) + " Punkte erforderlich");
        return e;
    }
    std::vector<CalPoint> s = pts;
    std::sort(s.begin(), s.end(), [](const CalPoint& a, const CalPoint& b) { return a.raw < b.raw; });
    int dir = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!(s[i].raw > s[i - 1].raw)) {
            e.emplace_back("Kennlinie: zwei Punkte mit gleichem Rohwert");
            break;
        }
        const double dm = s[i].mass_g - s[i - 1].mass_g;
        const int d = dm > 0 ? 1 : (dm < 0 ? -1 : 0);
        if (d == 0 || (dir != 0 && d != dir)) {
            e.emplace_back("Kennlinie: Masse nicht streng monoton über dem Rohwert");
            break;
        }
        dir = d;
    }
    return e;
}

/**
 * @brief Checks value ranges and dependencies of all parameters.
 * @return error messages; empty = valid
 */
std::vector<std::string> Config::validate() const {
    std::vector<std::string> e;
    if (ethercat.interface.empty()) e.emplace_back("ethercat.interface: leer");
    if (ethercat.cycle_ms < kCycleMsMin || ethercat.cycle_ms > kCycleMsMax)
        e.push_back(range_msg("ethercat.cycle_ms", kCycleMsMin, kCycleMsMax));
    if (ethercat.wkc_fault_cycles < kWkcFaultCyclesMin)
        e.push_back("ethercat.wkc_fault_cycles: >= " + std::to_string(kWkcFaultCyclesMin));
    if (ethercat.rt_priority < kRtPriorityMin || ethercat.rt_priority > kRtPriorityMax)
        e.push_back(range_msg("ethercat.rt_priority", kRtPriorityMin, kRtPriorityMax));

    if (device.nominal_load <= 0.0) e.emplace_back("device.nominal_load: > 0");
    if (device.reference_load <= 0.0) e.emplace_back("device.reference_load: > 0");
    if (device.rated_output_mv_v == 0.0) e.emplace_back("device.rated_output_mv_v: != 0");
    if (device.gravity_of_earth <= 0.0) e.emplace_back("device.gravity_of_earth: > 0");
    if (device.scale_factor == 0.0) e.emplace_back("device.scale_factor: != 0");
    for (auto& s : el3356::validate_device(device)) e.push_back(std::move(s));

    const auto& cal = calibration;
    if (cal.mode != "device" && cal.mode != "application")
        e.emplace_back("calibration.mode: 'device' oder 'application'");
    if (!valid_unit(cal.device_unit)) e.emplace_back("calibration.device_unit: mg, g, kg oder gr");
    if (cal.capture_time_ms < kCaptureMsMin || cal.capture_time_ms > kCaptureMsMax)
        e.push_back(range_msg("calibration.capture_time_ms", kCaptureMsMin, kCaptureMsMax));
    if (cal.stable_time_s < kStableTimeSMin || cal.stable_time_s > kStableTimeSMax)
        e.push_back(range_msg("calibration.stable_time_s", kStableTimeSMin, kStableTimeSMax));
    if (cal.stable_band_rel <= 0.0 || cal.stable_band_rel > kStableBandRelMax)
        e.push_back("calibration.stable_band_rel: zulässig >0.." + num_text(kStableBandRelMax));
    if (cal.mode == "application" && !cal.points.empty())
        for (auto& s : validate_points(cal.points)) e.push_back("calibration.points: " + s);

    const auto& m = measurement;
    if (!(m.min_load > m.zero_threshold))
        e.emplace_back("measurement.min_load muss > measurement.zero_threshold sein");
    if (m.tolerance <= 0.0) e.emplace_back("measurement.tolerance: > 0");
    if (m.sigma_max <= 0.0) e.emplace_back("measurement.sigma_max: > 0");
    if (m.stable_time_ms < kMinWindowCycles * ethercat.cycle_ms)
        e.push_back("measurement.stable_time_ms: >= " + std::to_string(kMinWindowCycles) + " * ethercat.cycle_ms");

    if (statistics.rolling_window < kRollingWindowMin || statistics.rolling_window > kRollingWindowMax)
        e.push_back(range_msg("statistics.rolling_window", kRollingWindowMin, kRollingWindowMax));
    if (statistics.history_size < kHistorySizeMin || statistics.history_size > kHistorySizeMax)
        e.push_back(range_msg("statistics.history_size", kHistorySizeMin, kHistorySizeMax));
    if (ui.rate_hz < kRateHzMin || ui.rate_hz > kRateHzMax)
        e.push_back(range_msg("ui.rate_hz", kRateHzMin, kRateHzMax));
    if (ui.display_average < kDisplayAverageMin || ui.display_average > kDisplayAverageMax)
        e.push_back(range_msg("ui.display_average", kDisplayAverageMin, kDisplayAverageMax));
    if (!valid_unit(ui.display_unit)) e.emplace_back("ui.display_unit: mg, g, kg oder gr");
    for (const auto& col : ui.hidden_columns)
        if (std::find(kMeasurementColumns.begin(), kMeasurementColumns.end(), col) == kMeasurementColumns.end())
            e.push_back("ui.hidden_columns: unbekannte Spalte '" + col + "'");
    if (http.port == 0) e.emplace_back("http.port: != 0");
    if (http.threads < kThreadsMin || http.threads > kThreadsMax)
        e.push_back(range_msg("http.threads", kThreadsMin, kThreadsMax));
    return e;
}

/**
 * @brief Loads and validates a configuration file.
 * @param path path to the JSON file
 * @return valid configuration
 * @throws std::runtime_error for an unreadable file, invalid JSON or invalid values
 */
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

/**
 * @brief Saves the configuration atomically (temporary file + rename).
 * @param path target path
 * @param c configuration
 * @throws std::runtime_error for a write or rename error
 */
void save_config(const std::string& path, const Config& c) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) throw std::runtime_error("Config nicht schreibbar: " + tmp);
        f << nlohmann::json(c).dump(kJsonIndent) << '\n';
        f.flush();
        if (!f) throw std::runtime_error("Schreibfehler: " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        throw std::runtime_error("rename fehlgeschlagen: " + tmp + " -> " + path);
}

/**
 * @brief Checks whether a change requires a restart of the service.
 * @param a previous configuration
 * @param b new configuration
 * @return true for changes to ethercat or http
 */
bool requires_restart(const Config& a, const Config& b) {
    return nlohmann::json(a.ethercat) != nlohmann::json(b.ethercat) ||
           nlohmann::json(a.http) != nlohmann::json(b.http);
}

/**
 * @brief Checks whether the terminal parameters written via SDO have changed.
 *
 * Cyclic outputs (sample_mode, disable_calibration, filter_frequency) and the
 * process data variant do not count.
 *
 * @param a previous configuration
 * @param b new configuration
 * @return true if device must be written to the terminal again
 */
bool device_config_changed(const Config& a, const Config& b) {
    auto sdo_part = [](const DeviceConfig& d) {
        nlohmann::json j = d;
        for (const char* k : {"pdo_variant", "sample_mode", "disable_calibration", "filter_frequency"}) j.erase(k);
        return j;
    };
    return sdo_part(a.device) != sdo_part(b.device);
}

/**
 * @brief Checks whether the PDO assignment (0x1C12/0x1C13) changes.
 * @param a previous configuration
 * @param b new configuration
 * @return true if the terminal must be reinitialized
 */
bool pdo_config_changed(const Config& a, const Config& b) {
    const auto pa = el3356::pdo_assignment(a.device);
    const auto pb = el3356::pdo_assignment(b.device);
    return pa.rx != pb.rx || pa.tx != pb.tx;
}

} // namespace ew
