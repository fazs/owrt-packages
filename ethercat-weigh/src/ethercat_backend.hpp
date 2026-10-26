#pragma once

#include <array>
#include <vector>

#include "el3356.hpp"
#include "io_backend.hpp"

namespace ew {
    /**
     * @brief EtherCAT-Master auf Basis von SOEM (Raw Socket, benötigt root bzw. CAP_NET_RAW).
     *
     * Topologie: EK1100 + EL3356-0010. Der Slave wird über seinen SII-Namen gefunden.
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

        Config cfg_; ///< stabile Kopie für den PO2SO-Hook (auch bei Rekonfiguration)
        static constexpr size_t kIomapSize = 4096U; ///< Größe des Prozessabbilds aller Slaves [Byte]
        std::array<uint8_t, kIomapSize> iomap_{}; ///< Prozessabbild aller Slaves
        bool opened_ = false; ///< ec_init erfolgreich
        uint16_t slave_ = 0; ///< Slave-Nummer der EL3356, 0 = nicht gefunden
        uint32_t revision_ = 0; ///< Revision für die Gerätebeschreibung, 0 = neueste (unbekannte Revision)
        int expected_wkc_ = 0; ///< erwarteter Working Counter
        uint32_t wkc_fail_count_ = 0; ///< aufeinanderfolgende WKC-Fehler
        uint32_t wkc_fault_cycles_ = EthercatConfig{}.wkc_fault_cycles; ///< WKC-Fehler bis Wiederanlauf
        bool link_ok_ = false; ///< alle Slaves in OP
        static constexpr double kNoCheck = -1.0; ///< last_check_t_: noch keine Prüfung
        double last_check_t_ = kNoCheck; ///< letzte Zustandsprüfung [s]
        el3356::InputLayout in_; ///< Lage der Eingänge (absolut im Slave-Abbild)
        el3356::OutputLayout out_; ///< Lage der Steuerbits
        std::vector<el3356::PdoInfo> pdos_; ///< PDO-Katalog (beim Hochlauf eingelesen)
    };
} // namespace ew
