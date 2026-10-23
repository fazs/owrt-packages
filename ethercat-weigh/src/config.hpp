#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ew {

struct EthercatConfig {
    std::string interface = "eth1";     // dedizierter EtherCAT-Port
    std::string slave_name = "EL3356-0010";
    uint32_t cycle_ms = 10;
    uint32_t wkc_fault_cycles = 3;      // aufeinanderfolgende WKC-Fehler bis Wiederanlauf
    int rt_priority = 0;                // 0 = SCHED_OTHER, 1..99 = SCHED_FIFO
};

// CoE-Objekt 0x8000 "RMB Settings" der EL3356-00x0.
// Lastwerte (nominal_load, reference_load) in calibration.device_unit.
struct DeviceConfig {
    bool write_on_start = true;
    bool mode0_enable_filter = true;       // 0x8000:01
    bool mode0_enable_averager = true;     // 0x8000:03
    uint16_t mode0_filter_settings = 0;    // 0x8000:11 (0 = FIR 50 Hz)
    double rated_output_mv_v = 2.0;        // 0x8000:23
    double nominal_load = 5.0;             // 0x8000:24
    double zero_balance = 0.0;             // 0x8000:25
    double gravity_of_earth = 9.80665;     // 0x8000:26
    double scale_factor = 1000.0;          // 0x8000:27
    double reference_load = 5.0;           // 0x8000:28
    uint16_t steady_state_window = 1000;   // 0x8000:29
    uint32_t steady_state_tolerance = 5;   // 0x8000:2A
    uint16_t calibration_interval = 1800;  // 0x8000:31
};

// Stützpunkt der Kennlinie (Kalibrierung in der Anwendung)
struct CalPoint {
    double raw = 0.0;     // Value (Real) der Klemme
    double mass_g = 0.0;  // zugeordnete Masse in Gramm
};

struct CalibrationConfig {
    // "device":      Kennlinie in der Klemme (0x8000:23/25); Masse = raw / scale_factor * device_unit
    // "application": Kennlinie aus points (stückweise linear, >= 2 Punkte)
    std::string mode = "device";
    std::string device_unit = "kg";   // Einheit von Nominal load / Reference load in der Klemme
    std::vector<CalPoint> points;
    uint32_t capture_time_ms = 2000;  // Mittelungszeit je Kalibrierpunkt (Anwendung)
    double stable_time_s = 10.0;      // Klemme: geforderter Stillstand vor Kommando
    double stable_band_rel = 0.0005;  // Klemme: Stillstandsband relativ zur Nennlast
};

// Alle Massen in Gramm
struct MeasurementConfig {
    double min_load = 100.0;            // ab hier gilt die Waage als belastet
    double zero_threshold = 20.0;       // darunter: Waage entlastet, Re-Arm
    uint32_t stable_time_ms = 500;      // Länge des Stabilitätsfensters
    double tolerance = 2.0;             // max. Spannweite (max - min) im Fenster
    double sigma_max = 1.0;             // max. Standardabweichung im Fenster
    bool use_device_steady_state = false; // zusätzlich Bit 0x6000:09 fordern
};

struct StatisticsConfig {
    uint32_t rolling_window = 50;
    uint32_t history_size = 500;        // gehaltene Messungen (RAM)
};

struct UiConfig {
    uint32_t rate_hz = 20;
    std::string display_unit = "g";     // Vorgabe für die Anzeige: mg, g, kg, gr
};

struct HttpConfig {
    std::string bind = "0.0.0.0";
    uint16_t port = 8080;
    std::string web_root = "/www/ethercat-weigh";
    uint16_t threads = 2;
};

struct Config {
    EthercatConfig ethercat;
    DeviceConfig device;
    CalibrationConfig calibration;
    MeasurementConfig measurement;
    StatisticsConfig statistics;
    UiConfig ui;
    HttpConfig http;

    // Leere Liste = gültig
    std::vector<std::string> validate() const;
};

void to_json(nlohmann::json& j, const CalPoint& p);
void from_json(const nlohmann::json& j, CalPoint& p);
void to_json(nlohmann::json& j, const Config& c);
void from_json(const nlohmann::json& j, Config& c);

// Unbekannte Schlüssel werden ignoriert, fehlende behalten den Default.
Config config_from_json(const nlohmann::json& j);
Config load_config(const std::string& path);           // wirft std::runtime_error
void save_config(const std::string& path, const Config& c); // atomar (tmp + rename)

// Prüft Stützpunkte: >= 2, raw paarweise verschieden, Masse streng monoton über raw.
std::vector<std::string> validate_points(const std::vector<CalPoint>& pts);

// Liefert true, wenn eine Änderung von a nach b einen Neustart erfordert.
bool requires_restart(const Config& a, const Config& b);
bool device_config_changed(const Config& a, const Config& b);

} // namespace ew
