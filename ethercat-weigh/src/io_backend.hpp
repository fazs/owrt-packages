#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config.hpp"
#include "el3356.hpp"
#include "sample.hpp"

namespace ew {

/**
 * @brief Interface between acquisition and process data source (EtherCAT or simulation).
 *
 * Pure virtual interface without definitions of its own, therefore documented here.
 * All methods are called exclusively from the acquisition thread.
 */
class IoBackend {
public:
    virtual ~IoBackend() = default;

    /// @return name of the source ("ethercat", "simulation")
    [[nodiscard]] virtual std::string name() const = 0;

    /**
     * @brief Initialization up to OP.
     * @param cfg configuration
     * @param[out] err cause on false
     * @return true if the slave is in OP
     */
    virtual bool open(const Config& cfg, std::string& err) = 0;

    /**
     * @brief One process data cycle: write outputs, read inputs.
     * @param t time since start [s]
     * @param ctrl control bits 0x7000
     * @param[out] out sets t, raw (Value (Real)), status, wkc_ok, link_ok
     */
    virtual void cycle(double t, const El3356Control& ctrl, Sample& out) = 0;

    /**
     * @brief Writes the CoE parameters 0x8000 (mailbox, also allowed in OP).
     * @param dc terminal parameters
     * @param[out] err error text
     * @return true on success
     */
    virtual bool write_device_config(const DeviceConfig& dc, std::string& err) = 0;

    /**
     * @brief Single SDO write access (mailbox).
     * @param index object index
     * @param sub subindex
     * @param data data, little endian
     * @param[out] err error text
     * @return true on success
     */
    virtual bool sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                           std::string& err) = 0;

    /**
     * @brief Single SDO read access (mailbox).
     * @param index object index
     * @param sub subindex
     * @param size buffer size (at least the object size)
     * @param[out] data data read, truncated to the actual length
     * @param[out] err error text
     * @return true on success
     */
    virtual bool sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                          std::string& err) = 0;

    /// @brief Closes the connection (slave to INIT).
    virtual void close() = 0;

    /**
     * @brief Debug: all PDOs of the terminal.
     * @return PDO catalog; assigned entries with their position in the slave's process image
     */
    [[nodiscard]] virtual std::vector <el3356::PdoInfo> pdos() const = 0;

    /**
     * @brief Debug: process image of the slave after the last cycle.
     * @param[out] in input image
     * @param[out] out output image
     */
    virtual void process_image(std::vector<uint8_t>& in, std::vector<uint8_t>& out) const = 0;

    /**
     * @brief Simulation only: set the applied mass.
     * @param grams mass [g], std::nullopt = automatic load profile
     * @return false if the source is not a simulation
     */
    virtual bool set_sim_load(std::optional<double> /*grams*/) { return false; }
};

} // namespace ew
