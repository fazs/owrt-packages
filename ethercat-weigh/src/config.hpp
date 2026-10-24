#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ew {

/// EtherCAT-Master (wirkt nach Neustart).
struct EthercatConfig {
    std::string interface = "eth1";     ///< dedizierter EtherCAT-Port
    std::string slave_name = "EL3356-0010"; ///< SII-Name des Slaves (Präfix)
    uint32_t cycle_ms = 10;             ///< Zykluszeit
    uint32_t wkc_fault_cycles = 3;      ///< aufeinanderfolgende WKC-Fehler bis Wiederanlauf
    int rt_priority = 0;                ///< 0 = SCHED_OTHER, 1..99 = SCHED_FIFO
};

/**
 * @brief CoE-Objekt 0x8000 "RMB Settings" der EL3356-00x0.
 *
 * Lastwerte (nominal_load, reference_load) in calibration.device_unit.
 */
struct DeviceConfig {
    bool write_on_start = true;            ///< beim Start in die Klemme schreiben
    bool mode0_enable_filter = true;       ///< 0x8000:01
    bool mode0_enable_averager = true;     ///< 0x8000:03
    uint16_t mode0_filter_settings = 0;    ///< 0x8000:11 (0 = FIR 50 Hz)
    double rated_output_mv_v = 0.9;        ///< 0x8000:23 Kennwert [mV/V]
    double nominal_load = 50.0;            ///< 0x8000:24 Nennlast
    double zero_balance = 0.0;             ///< 0x8000:25 Nullpunkt [mV/V]
    double gravity_of_earth = 9.80665;     ///< 0x8000:26 [m/s²]
    double scale_factor = 1000.0;          ///< 0x8000:27
    double reference_load = 50.0;          ///< 0x8000:28 Referenzlast
    uint16_t steady_state_window = 1000;   ///< 0x8000:29 [ms]
    uint32_t steady_state_tolerance = 5;   ///< 0x8000:2A
    uint16_t calibration_interval = 1800;  ///< 0x8000:31
};

/// Stützpunkt der Kennlinie (Kalibrierung in der Anwendung).
struct CalPoint {
    double raw = 0.0;     ///< Value (Real) der Klemme
    double mass_g = 0.0;  ///< zugeordnete Masse in Gramm
};

/// Kalibrierverfahren und Kennlinie.
struct CalibrationConfig {
    /// "device": Kennlinie in der Klemme (0x8000:23/25), Masse = raw / scale_factor * device_unit;
    /// "application": Kennlinie aus points (stückweise linear, >= 2 Punkte)
    std::string mode = "device";
    std::string device_unit = "g";    ///< Einheit von Nominal load / Reference load in der Klemme
    std::vector<CalPoint> points;     ///< Stützpunkte (application)
    uint32_t capture_time_ms = 2000;  ///< Mittelungszeit je Kalibrierpunkt (Anwendung)
    double stable_time_s = 10.0;      ///< Klemme: geforderter Stillstand vor Kommando
    double stable_band_rel = 0.0005;  ///< Klemme: Stillstandsband relativ zur Nennlast
};

/// Erkennung gültiger Messungen; alle Massen in Gramm.
struct MeasurementConfig {
    double min_load = 3.0;              ///< ab hier gilt die Waage als belastet
    double zero_threshold = 1.0;        ///< darunter: Waage entlastet, neue Messung freigegeben
    uint32_t stable_time_ms = 500;      ///< Länge des Stabilitätsfensters
    double tolerance = 2.0;             ///< max. Spannweite (max - min) im Fenster
    double sigma_max = 1.0;             ///< max. Standardabweichung im Fenster
    bool use_device_steady_state = false; ///< zusätzlich Bit 0x6000:09 fordern
};

/// Temporäre Statistik.
struct StatisticsConfig {
    uint32_t rolling_window = 50;       ///< Größe des gleitenden Fensters
    uint32_t history_size = 500;        ///< gehaltene Messungen (RAM)
};

/// Anzeige.
struct UiConfig {
    uint32_t rate_hz = 20;              ///< Rate der sample-Nachrichten
    std::string display_unit = "g";     ///< Vorgabe für die Anzeige: mg, g, kg, gr
};

/// Webserver (wirkt nach Neustart).
struct HttpConfig {
    std::string bind = "0.0.0.0";       ///< Adresse
    uint16_t port = 8080;               ///< Port
    std::string web_root = "/www/ethercat-weigh"; ///< Verzeichnis des Frontends
    uint16_t threads = 2;               ///< Worker-Threads
};

/// Gesamte Konfiguration (/etc/ethercat-weigh/config.json).
struct Config {
    EthercatConfig ethercat;        ///< EtherCAT-Master
    DeviceConfig device;            ///< Klemmenparameter 0x8000
    CalibrationConfig calibration;  ///< Kalibrierung
    MeasurementConfig measurement;  ///< Messalgorithmus
    StatisticsConfig statistics;    ///< Statistik
    UiConfig ui;                    ///< Anzeige
    HttpConfig http;                ///< Webserver

    [[nodiscard]] std::vector<std::string> validate() const;
};

void to_json(nlohmann::json& j, const CalPoint& p);
void from_json(const nlohmann::json& j, CalPoint& p);
void to_json(nlohmann::json& j, const Config& c);
void from_json(const nlohmann::json& j, Config& c);

Config config_from_json(const nlohmann::json& j);
Config load_config(const std::string& path);
void save_config(const std::string& path, const Config& c);

std::vector<std::string> validate_points(const std::vector<CalPoint>& pts);

bool requires_restart(const Config& a, const Config& b);
bool device_config_changed(const Config& a, const Config& b);

} // namespace ew
