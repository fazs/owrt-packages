#pragma once

#include <array>
#include <vector>

#include "el3356.hpp"
#include "io_backend.hpp"

namespace ew {
    /**
     * @brief EtherCAT master based on SOEM (raw socket, requires root or CAP_NET_RAW).
     *
     * Topology: EK1100 + EL3356-0010. The slave is found by its SII name.
     */
    class EthercatBackend : public IoBackend {
    public:
        EthercatBackend() = default;

        // ReSharper disable once Cpp[[nodiscard]] VirtualFunctionCallInsideCtor
        ~EthercatBackend() override { close(); }

        std::string name() const override { return "ethercat"; }

        bool open(const Config &cfg, std::string &err) override;

        void cycle(double t, const El3356Control &ctrl, Sample &out) override;

        bool write_device_config(const DeviceConfig &dc, std::string &err) override;

        bool sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t> &data,
                       std::string &err) override;

        [[nodiscard]] bool sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t> &data,
                      std::string &err) override;

        void close() override;

        [[nodiscard]] std::vector<el3356::PdoInfo> pdos() const override { return pdos_; }

        void process_image(std::vector<uint8_t> &in, std::vector<uint8_t> &out) const override;

    private:
        bool read_layout(std::string &err);

        void discover_pdos(const std::vector<uint16_t> &rx_assign,
                           const std::vector<uint16_t> &tx_assign);

        void check_and_recover(double t);

        Config cfg_; ///< stable copy for the PO2SO hook (also during reconfiguration)
        static constexpr size_t kIomapSize = 4096U; ///< size of the process image of all slaves [bytes]
        std::array<uint8_t, kIomapSize> iomap_{}; ///< process image of all slaves
        bool opened_ = false; ///< ec_init successful
        uint16_t slave_ = 0; ///< slave number of the EL3356, 0 = not found
        uint32_t revision_ = 0; ///< revision for the device description, 0 = latest (unknown revision)
        int expected_wkc_ = 0; ///< expected working counter
        uint32_t wkc_fail_count_ = 0; ///< consecutive WKC errors
        uint32_t wkc_fault_cycles_ = EthercatConfig{}.wkc_fault_cycles; ///< WKC errors until restart
        bool link_ok_ = false; ///< all slaves in OP
        static constexpr double kNoCheck = -1.0; ///< last_check_t_: no check yet
        double last_check_t_ = kNoCheck; ///< last state check [s]
        el3356::InputLayout in_; ///< position of the inputs (absolute in the slave image)
        el3356::OutputLayout out_; ///< position of the control bits
        std::vector<el3356::PdoInfo> pdos_; ///< PDO catalog (read during startup)
    };
} // namespace ew
