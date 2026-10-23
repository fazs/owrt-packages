#pragma once

#include <array>
#include <cstdint>

#include "el3356.hpp"
#include "io_backend.hpp"

namespace ew {

// EtherCAT-Master auf Basis von SOEM (Raw Socket, benötigt root bzw. CAP_NET_RAW).
// Topologie: EK1100 + EL3356-0010. Der Slave wird über seinen SII-Namen gefunden.
class EthercatBackend : public IoBackend {
public:
    EthercatBackend() = default;
    ~EthercatBackend() override { close(); }

    std::string name() const override { return "ethercat"; }
    bool open(const Config& cfg, std::string& err) override;
    void cycle(double t, const El3356Control& ctrl, Sample& out) override;
    bool write_device_config(const DeviceConfig& dc, std::string& err) override;
    bool sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                   std::string& err) override;
    bool sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                  std::string& err) override;
    void close() override;

private:
    bool read_layout(std::string& err);
    void check_and_recover(double t);

    Config cfg_;  // stabile Kopie für den PO2SO-Hook (auch bei Rekonfiguration)
    std::array<uint8_t, 4096> iomap_{};
    bool opened_ = false;
    uint16_t slave_ = 0;
    int expected_wkc_ = 0;
    uint32_t wkc_fail_count_ = 0;
    uint32_t wkc_fault_cycles_ = 3;
    bool link_ok_ = false;
    double last_check_t_ = -1.0;
    el3356::InputLayout in_;
    el3356::OutputLayout out_;
};

} // namespace ew
