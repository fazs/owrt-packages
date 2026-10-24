#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"
#include "el3356.hpp"
#include "sample.hpp"

namespace ew {

/**
 * @brief Schnittstelle zwischen Acquisition und Prozessdatenquelle (EtherCAT oder Simulation).
 *
 * Rein virtuelle Schnittstelle ohne eigene Definitionen, daher hier dokumentiert.
 * Alle Methoden werden ausschließlich aus dem Acquisition-Thread aufgerufen.
 */
class IoBackend {
public:
    virtual ~IoBackend() = default;

    /// @return Name der Quelle ("ethercat", "simulation")
    virtual std::string name() const = 0;

    /**
     * @brief Initialisierung bis OP.
     * @param cfg Konfiguration
     * @param[out] err Ursache bei false
     * @return true, wenn der Slave in OP ist
     */
    virtual bool open(const Config& cfg, std::string& err) = 0;

    /**
     * @brief Ein Prozessdatenzyklus: Ausgänge schreiben, Eingänge lesen.
     * @param t Zeit seit Start [s]
     * @param ctrl Steuerbits 0x7000
     * @param[out] out setzt t, raw (Value (Real)), status, wkc_ok, link_ok
     */
    virtual void cycle(double t, const El3356Control& ctrl, Sample& out) = 0;

    /**
     * @brief Schreibt die CoE-Parameter 0x8000 (Mailbox, auch in OP zulässig).
     * @param dc Klemmenparameter
     * @param[out] err Fehlertext
     * @return true bei Erfolg
     */
    virtual bool write_device_config(const DeviceConfig& dc, std::string& err) = 0;

    /**
     * @brief Einzelner SDO-Schreibzugriff (Mailbox).
     * @param index Objektindex
     * @param sub Subindex
     * @param data Daten, little endian
     * @param[out] err Fehlertext
     * @return true bei Erfolg
     */
    virtual bool sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                           std::string& err) = 0;

    /**
     * @brief Einzelner SDO-Lesezugriff (Mailbox).
     * @param index Objektindex
     * @param sub Subindex
     * @param size Puffergröße (mindestens die Objektgröße)
     * @param[out] data gelesene Daten, auf die tatsächliche Länge gekürzt
     * @param[out] err Fehlertext
     * @return true bei Erfolg
     */
    virtual bool sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                          std::string& err) = 0;

    /// @brief Beendet die Verbindung (Slave nach INIT).
    virtual void close() = 0;

    /**
     * @brief Debug: alle PDOs der Klemme.
     * @return PDO-Katalog; zugeordnete Einträge mit Lage im Prozessabbild des Slaves
     */
    virtual std::vector<el3356::PdoInfo> pdos() const = 0;

    /**
     * @brief Debug: Prozessabbild des Slaves nach dem letzten Zyklus.
     * @param[out] in Eingangsabbild
     * @param[out] out Ausgangsabbild
     */
    virtual void process_image(std::vector<uint8_t>& in, std::vector<uint8_t>& out) const = 0;

    /**
     * @brief Nur Simulation: aufgelegte Masse vorgeben.
     * @param grams Masse [g], std::nullopt = automatisches Lastprofil
     * @return false, wenn die Quelle keine Simulation ist
     */
    virtual bool set_sim_load(std::optional<double> /*grams*/) { return false; }
};

} // namespace ew
