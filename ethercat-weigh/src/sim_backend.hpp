#pragma once

#include <map>
#include <optional>
#include <random>

#include "io_backend.hpp"

namespace ew {

/**
 * @brief Simulated EL3356 for development and testing without hardware.
 *
 * - Sensor: bridge signal [mV/V] = zero + mass / capacity * rated + noise (see SimSensor)
 * - Terminal: Value = ((signal - zero balance) / rated output * nominal load * gain - tare) * scale factor
 * - Commands 0xFB00:01: 0x0101 zero balance, 0x0102 calibration with reference load,
 *   0x0001/0x0002 tare; 0x1011:01 "load" resets gain and tare
 * - Diagnostics: 0x9000:11 = bridge signal, 0xA000 without messages
 * - Load profile (cyclic): empty -> load applied (damped settling) -> hold -> unload,
 *   or a fixed mass via set_sim_load()
 */
class SimBackend : public IoBackend {
public:
    explicit SimBackend(unsigned seed = 1);
    [[nodiscard]] std::string name() const override { return "simulation"; }
    bool open(const Config& cfg, std::string& err) override;
    void cycle(double t, const El3356Control& ctrl, Sample& out) override;
    bool write_device_config(const DeviceConfig& dc, std::string& err) override;
    bool sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                   std::string& err) override;
    bool sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                  std::string& err) override;
    void close() override {}
    bool set_sim_load(std::optional<double> grams) override;
    [[nodiscard]] std::vector<el3356::PdoInfo> pdos() const override { return pdos_; }
    void process_image(std::vector<uint8_t>& in, std::vector<uint8_t>& out) const override {
        in = img_in_;
        out = img_out_;
    }

    /// @name Model parameters of the simulation (also used by the tests)
    /// @{
    static constexpr double kProfileEmptyS = 2.0;    ///< load profile: empty [s]
    static constexpr double kProfileLoadedS = 5.0;   ///< load profile: loaded incl. settling [s]
    static constexpr double kProfilePeriodS = kProfileEmptyS + kProfileLoadedS;  ///< period [s]
    static constexpr double kLoadFractionMin = 0.1;  ///< load profile: smallest load (fraction of capacity)
    static constexpr double kLoadFractionMax = 0.9;  ///< load profile: largest load (fraction of capacity)
    static constexpr double kSettleFrequencyHz = 3.0;  ///< settling: frequency of the oscillation
    static constexpr double kSettleTauS = 0.25;        ///< settling: decay time constant [s]
    static constexpr double kOverrangeFactor = 1.1;  ///< overrange from this multiple of the nominal load
    static constexpr double kSteadyBandRel = 0.001;  ///< steady state: band relative to the nominal load
    static constexpr double kSteadyTimeS = 1.0;      ///< steady state: minimum duration within the band [s]
    static constexpr double kRatedDev = 0.005;  ///< rated value +0.5 % compared to the nominal value
    static constexpr double kZeroDev = 0.01;    ///< zero point +1 % of the rated value
    static constexpr double kNoiseRel = 6e-5;   ///< noise relative to the rated value (6e-5 · capacity)
    static constexpr double kIir8NoiseFactor = 0.3;  ///< noise reduction by filter IIR8
    static constexpr int kRatedSigDigits = 2;        ///< nominal rated output: significant digits
    static constexpr double kZeroStepRel = 0.05;     ///< nominal zero balance: grid relative to the rated value
    static constexpr double kFallbackCapacityG = 1000.0;  ///< capacity for an invalid nominal load [g]
    static constexpr double kFallbackRatedMvV = 1.0;      ///< rated value for rated output 0 [mV/V]
    static constexpr int kCmdBusyReads = 3;  ///< status polls of 0xFB00:02 until a command completes
    static constexpr uint8_t kCmdStatusUnknown = 1U;  ///< 0xFB00:02 for an unknown command
    /// @}

    /// Simulated sensor, derived from the config (see sensor_for()).
    struct SimSensor {
        double capacity_kg;  ///< capacity = nominal load
        double rated_mv_v;   ///< rated value [mV/V] at capacity
        double zero_mv_v;    ///< zero point [mV/V]
        double noise_mv_v;   ///< standard deviation of the noise without filter
    };
    static SimSensor sensor_for(const Config& cfg);
    /// currently simulated sensor (after open() from the config)
    [[nodiscard]] const SimSensor& sensor() const { return sensor_; }

private:
    void build_image(const DeviceConfig& dc);
    double mass_kg(double t);
    double signal_mvv(double mass);

    std::mt19937 rng_;                                            ///< random generator (fixed seed)
    std::normal_distribution<double> noise_{0.0, 1.0};            ///< noise, normalized
    std::uniform_real_distribution<double> load_dist_{kLoadFractionMin, kLoadFractionMax};  ///< load as a fraction of capacity
    SimSensor sensor_;                                            ///< simulated sensor

    // Object dictionary (excerpt); values from DeviceConfig (constructor, open(), write_device_config())
    float gain_ = 0.0F;        ///< 0x8000:21 Gain
    float tara_ = 0.0F;        ///< 0x8000:22 Tara
    float rated_ = 0.0F;       ///< 0x8000:23 Rated output
    float nominal_ = 0.0F;     ///< 0x8000:24 Nominal load
    float zero_bal_ = 0.0F;    ///< 0x8000:25 Zero balance
    float scale_ = 0.0F;       ///< 0x8000:27 Scale factor
    float reference_ = 0.0F;   ///< 0x8000:28 Reference load
    uint16_t filter_ = 0U;     ///< 0x8000:11 Mode0 filter settings
    uint16_t cmd_ = el3356::kCmdNone;              ///< 0xFB00:01 Request
    uint8_t cmd_status_ = el3356::kCmdStatusDone;  ///< 0xFB00:02 Status
    int cmd_busy_reads_ = 0;   ///< status polls until the command completes

    /// Other objects: stored SDO data (key see od_key())
    std::map<uint32_t, std::vector<uint8_t>> od_;

    // Process image according to the assignment from the configuration (el3356::pdo_assignment)
    std::vector<el3356::PdoInfo> pdos_;  ///< PDO catalog
    el3356::InputLayout in_;             ///< position of the inputs
    el3356::OutputLayout out_;           ///< position of the control bits
    std::vector<uint8_t> img_in_;        ///< input image
    std::vector<uint8_t> img_out_;       ///< output image

    std::optional<double> fixed_kg_;  ///< fixed mass, std::nullopt = load profile
    double last_signal_ = 0.0;        ///< last bridge signal [mV/V]
    double phase_start_ = 0.0;        ///< start of the current profile period [s]
    double target_ = 0.0;             ///< load of the current profile period [kg]
    double load_since_ = 0.0;         ///< start of the fixed load [s], < 0 = settle again
    double steady_ref_ = 0.0;         ///< reference value for steady state
    double steady_since_ = 0.0;       ///< start of standstill [s]
    bool toggle_ = false;             ///< TxPDO Toggle
};

} // namespace ew
