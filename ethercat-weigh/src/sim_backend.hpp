#pragma once

#include <map>
#include <optional>
#include <random>

#include "io_backend.hpp"

namespace ew {

/**
 * @brief Simulierte EL3356 für Entwicklung und Test ohne Hardware.
 *
 * - Sensor: Brückensignal [mV/V] = zero + Masse / Kapazität * rated + Rauschen (siehe SimSensor)
 * - Klemme: Value = ((Signal - Zero balance) / Rated output * Nominal load * Gain - Tara) * Scale factor
 * - Kommandos 0xFB00:01: 0x0101 Nullabgleich, 0x0102 Kalibrierung mit Reference load
 * - Lastprofil (zyklisch): leer -> Last aufgelegt (gedämpftes Einschwingen) -> halten -> entlasten,
 *   oder feste Masse über set_sim_load()
 */
class SimBackend : public IoBackend {
public:
    explicit SimBackend(unsigned seed = 1);
    std::string name() const override { return "simulation"; }
    bool open(const Config& cfg, std::string& err) override;
    void cycle(double t, const El3356Control& ctrl, Sample& out) override;
    bool write_device_config(const DeviceConfig& dc, std::string& err) override;
    bool sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                   std::string& err) override;
    bool sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                  std::string& err) override;
    void close() override {}
    bool set_sim_load(std::optional<double> grams) override;
    std::vector<el3356::PdoInfo> pdos() const override { return pdos_; }
    void process_image(std::vector<uint8_t>& in, std::vector<uint8_t>& out) const override {
        in = img_in_;
        out = img_out_;
    }

    /// Simulierter Sensor, aus der Config abgeleitet (siehe sensor_for()).
    struct SimSensor {
        double capacity_kg;  ///< Kapazität = Nominal load
        double rated_mv_v;   ///< Kennwert [mV/V] bei Kapazität
        double zero_mv_v;    ///< Nullpunkt [mV/V]
        double noise_mv_v;   ///< Standardabweichung des Rauschens ohne Filter
    };
    static SimSensor sensor_for(const Config& cfg);
    /// aktuell simulierter Sensor (nach open() aus der Config)
    const SimSensor& sensor() const { return sensor_; }

private:
    double mass_kg(double t);
    double signal_mvv(double mass);

    std::mt19937 rng_;                                            ///< Zufallsgenerator (Seed fest)
    std::normal_distribution<double> noise_{0.0, 1.0};            ///< Rauschen, normiert
    std::uniform_real_distribution<double> load_dist_{0.1, 0.9};  ///< Last als Anteil der Kapazität
    SimSensor sensor_;                                            ///< simulierter Sensor

    // Objektverzeichnis (Auszug); Startwerte werden in open() überschrieben
    float gain_ = 1.0f;       ///< 0x8000:21 Gain
    float tara_ = 0.0f;       ///< 0x8000:22 Tara
    float rated_ = 2.0f;      ///< 0x8000:23 Rated output
    float nominal_ = 5.0f;    ///< 0x8000:24 Nominal load
    float zero_bal_ = 0.0f;   ///< 0x8000:25 Zero balance
    float scale_ = 1000.0f;   ///< 0x8000:27 Scale factor
    float reference_ = 5.0f;  ///< 0x8000:28 Reference load
    uint16_t filter_ = 0;     ///< 0x8000:11 Mode0 filter settings
    uint16_t cmd_ = 0;        ///< 0xFB00:01 Request
    uint8_t cmd_status_ = 0;  ///< 0xFB00:02 Status
    int cmd_busy_reads_ = 0;  ///< Statusabfragen bis zum Abschluss des Kommandos

    /// Übrige Objekte: gespeicherte SDO-Daten (Schlüssel index << 8 | sub)
    std::map<uint32_t, std::vector<uint8_t>> od_;

    // Prozessabbild gemäß Standard-Zuordnung (el3356::rx_assignment/tx_assignment)
    std::vector<el3356::PdoInfo> pdos_;  ///< PDO-Katalog
    el3356::InputLayout in_;             ///< Lage der Eingänge
    el3356::OutputLayout out_;           ///< Lage der Steuerbits
    std::vector<uint8_t> img_in_;        ///< Eingangsabbild
    std::vector<uint8_t> img_out_;       ///< Ausgangsabbild

    std::optional<double> fixed_kg_;  ///< feste Masse, std::nullopt = Lastprofil
    double last_signal_ = 0.0;        ///< letztes Brückensignal [mV/V]
    double phase_start_ = 0.0;        ///< Beginn der aktuellen Profilperiode [s]
    double target_ = 0.0;             ///< Last der aktuellen Profilperiode [kg]
    double load_since_ = 0.0;         ///< Beginn der festen Last [s], < 0 = neu einschwingen
    double steady_ref_ = 0.0;         ///< Bezugswert für Steady state
    double steady_since_ = 0.0;       ///< Beginn des Stillstands [s]
    bool toggle_ = false;             ///< TxPDO Toggle
};

} // namespace ew
