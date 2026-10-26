#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ew {

/// EtherCAT master (takes effect after restart).
struct EthercatConfig {
    std::string interface = "eth1";     ///< dedicated EtherCAT port
    std::string slave_name = "EL3356-0010"; ///< SII name of the slave (prefix)
    uint32_t cycle_ms = 10;             ///< cycle time
    uint32_t wkc_fault_cycles = 3;      ///< consecutive WKC errors until restart
    int rt_priority = 0;                ///< 0 = SCHED_OTHER, 1..99 = SCHED_FIFO
};

/**
 * @brief Terminal parameters of the EL3356-00x0 (CoE 0x8000 "RMB Settings", 0x8010/0x8020 "AI Settings").
 *
 * The typed fields are used by the characteristic, simulation and calibration; their
 * mapping to CoE entries is in the device description (config_key). Load values
 * (nominal_load, reference_load) in calibration.device_unit.
 */
struct DeviceConfig {
    bool write_on_start = true;            ///< write to the terminal at startup
    bool mode0_enable_filter = true;       ///< 0x8000:01
    bool mode0_enable_averager = true;     ///< 0x8000:03
    uint16_t mode0_filter_settings = 0;    ///< 0x8000:11 (0 = FIR 50 Hz)
    double rated_output_mv_v = 0.9;        ///< 0x8000:23 rated output [mV/V]
    double nominal_load = 50.0;            ///< 0x8000:24 nominal load
    double zero_balance = 0.0;             ///< 0x8000:25 zero point [mV/V]
    double gravity_of_earth = 9.80665;     ///< 0x8000:26 [m/s²]
    double scale_factor = 1000.0;          ///< 0x8000:27
    double reference_load = 50.0;          ///< 0x8000:28 reference load
    uint16_t steady_state_window = 1000;   ///< 0x8000:29 [ms]
    uint32_t steady_state_tolerance = 5;   ///< 0x8000:2A
    uint16_t calibration_interval = 1800;  ///< 0x8000:31
    /// Other writable terminal parameters (0x8000, 0x8010, 0x8020) as CoE raw value, key
    /// "0xIIII:SS"; entries not listed get the ESI default when writing
    std::map<std::string, double> coe;
    /// Process data: "real" (Standard (REAL), 0x1A02) or "int32" (Standard (INT32), 0x1A01,
    /// experimental); a change reinitializes the terminal
    std::string pdo_variant = "real";
    bool sample_mode = false;          ///< 0x7000:04 sample mode (cyclic output)
    bool disable_calibration = false;  ///< 0x7000:02 disable calibration (cyclic output)
    uint16_t filter_frequency = 0;     ///< 0x7000:11 filter frequency [0.1 Hz], effective with filter 11
};

/// Calibration point of the characteristic (calibration in the application).
struct CalPoint {
    double raw = 0.0;     ///< Value (Real) of the terminal
    double mass_g = 0.0;  ///< assigned mass in grams
};

/// Calibration method and characteristic.
struct CalibrationConfig {
    /// "device": characteristic in the terminal (0x8000:23/25), mass = raw / scale_factor * device_unit;
    /// "application": characteristic from points (piecewise linear, >= 2 points)
    std::string mode = "device";
    std::string device_unit = "g";    ///< unit of nominal load / reference load in the terminal
    std::vector<CalPoint> points;     ///< calibration points (application)
    uint32_t capture_time_ms = 2000;  ///< averaging time per calibration point (application)
    double stable_time_s = 10.0;      ///< terminal: required standstill before the command
    double stable_band_rel = 0.0005;  ///< terminal: standstill band relative to the nominal load
};

/// Detection of valid measurements; all masses in grams.
struct MeasurementConfig {
    double min_load = 3.0;              ///< from here on the scale counts as loaded
    double zero_threshold = 1.0;        ///< below: scale unloaded, new measurement armed
    uint32_t stable_time_ms = 500;      ///< length of the stability window
    double tolerance = 2.0;             ///< max. range (max - min) in the window
    double sigma_max = 1.0;             ///< max. standard deviation in the window
    bool use_device_steady_state = false; ///< additionally require bit 0x6000:09
};

/// Temporary statistics.
struct StatisticsConfig {
    uint32_t rolling_window = 50;       ///< size of the rolling window
    uint32_t history_size = 500;        ///< held measurements (RAM)
};

/// Columns of the measurement table in the frontend (keys as in web/src/lib/Measurements.svelte).
constexpr std::array<std::string_view, 6> kMeasurementColumns = {
    "pos", "id", "value", "stddev", "settle_ms", "t"};

/// Display.
struct UiConfig {
    uint32_t rate_hz = 20;              ///< rate of the sample messages
    uint32_t display_average = 10;      ///< moving average of the display value [cycles], 1 = off
    std::string display_unit = "g";     ///< default for the display: mg, g, kg, gr
    std::vector<std::string> hidden_columns; ///< hidden columns (kMeasurementColumns)
};

/// Web server (takes effect after restart).
struct HttpConfig {
    std::string bind = "0.0.0.0";       ///< address
    uint16_t port = 8080;               ///< Port
    std::string web_root = "/www/ethercat-weigh"; ///< directory of the frontend
    uint16_t threads = 2;               ///< worker threads
};

/// Complete configuration (/etc/ethercat-weigh/config.json).
struct Config {
    EthercatConfig ethercat;        ///< EtherCAT master
    DeviceConfig device;            ///< terminal parameters 0x8000
    CalibrationConfig calibration;  ///< calibration
    MeasurementConfig measurement;  ///< measurement algorithm
    StatisticsConfig statistics;    ///< statistics
    UiConfig ui;                    ///< display
    HttpConfig http;                ///< web server

    [[nodiscard]] std::vector<std::string> validate() const;
};

void to_json(nlohmann::json& j, const CalPoint& p);
void from_json(const nlohmann::json& j, CalPoint& p);
void to_json(nlohmann::json& j, const Config& c);
void from_json(const nlohmann::json& j, Config& c);

Config config_from_json(const nlohmann::json& j);
nlohmann::json device_json(const DeviceConfig& dc);
Config load_config(const std::string& path);
void save_config(const std::string& path, const Config& c);

std::vector<std::string> validate_points(const std::vector<CalPoint>& pts);

bool requires_restart(const Config& a, const Config& b);
bool device_config_changed(const Config& a, const Config& b);
bool pdo_config_changed(const Config& a, const Config& b);

} // namespace ew
