#pragma once

#include <optional>
#include <random>

#include "io_backend.hpp"

namespace ew {

// Simulierte EL3356 für Entwicklung und Test ohne Hardware.
//
// Sensor: Brückensignal [mV/V] = zero_true + Masse/5 kg * rated_true + Rauschen
// Klemme: Value = ((Signal - Zero balance) / Rated output * Nominal load * Gain - Tara) * Scale factor
// Kommandos 0xFB00:01: 0x0101 Nullabgleich, 0x0102 Kalibrierung mit Reference load.
// Lastprofil (zyklisch): leer -> Last aufgelegt (gedämpftes Einschwingen) -> halten -> entlasten,
// oder feste Masse über set_sim_load().
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

    // Sensorparameter (für Tests)
    static constexpr double kZeroTrue = 0.0123;    // mV/V
    static constexpr double kRatedTrue = 1.9876;   // mV/V bei 5 kg
    static constexpr double kCapacityKg = 5.0;

private:
    double mass_kg(double t);
    double signal_mvv(double mass);

    std::mt19937 rng_;
    std::normal_distribution<double> noise_{0.0, 1.0};
    std::uniform_real_distribution<double> load_dist_{0.5, 4.5};

    // Objektverzeichnis (Auszug)
    float gain_ = 1.0f, tara_ = 0.0f, rated_ = 2.0f, nominal_ = 5.0f, zero_bal_ = 0.0f;
    float scale_ = 1000.0f, reference_ = 5.0f;
    uint16_t filter_ = 0;
    uint16_t cmd_ = 0;
    uint8_t cmd_status_ = 0;
    int cmd_busy_reads_ = 0;

    std::optional<double> fixed_kg_;
    double last_signal_ = 0.0;
    double phase_start_ = 0.0;
    double target_ = 0.0;
    double load_since_ = 0.0;
    double steady_ref_ = 0.0;
    double steady_since_ = 0.0;
    bool toggle_ = false;
};

} // namespace ew
