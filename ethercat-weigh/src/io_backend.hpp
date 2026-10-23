#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"
#include "el3356.hpp"
#include "sample.hpp"

namespace ew {

// Schnittstelle zwischen Acquisition und Prozessdatenquelle (EtherCAT oder Simulation).
// Alle Methoden werden ausschließlich aus dem Acquisition-Thread aufgerufen.
class IoBackend {
public:
    virtual ~IoBackend() = default;
    virtual std::string name() const = 0;
    // Initialisierung bis OP. Bei false steht die Ursache in err.
    virtual bool open(const Config& cfg, std::string& err) = 0;
    // Ein Prozessdatenzyklus: Ausgänge schreiben, Eingänge lesen.
    // Setzt t, raw (Value (Real)), status, wkc_ok, link_ok.
    virtual void cycle(double t, const El3356Control& ctrl, Sample& out) = 0;
    // CoE-Parameter 0x8000 schreiben (Mailbox, auch in OP zulässig).
    virtual bool write_device_config(const DeviceConfig& dc, std::string& err) = 0;
    // Einzelner SDO-Zugriff (Mailbox). Bei sdo_read gibt size die erwartete Länge vor.
    virtual bool sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                           std::string& err) = 0;
    virtual bool sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                          std::string& err) = 0;
    virtual void close() = 0;

    // Debug: alle PDOs der Klemme; zugeordnete mit Lage im Prozessabbild des Slaves
    virtual std::vector<el3356::PdoInfo> pdos() const = 0;
    // Debug: Prozessabbild des Slaves nach dem letzten Zyklus
    virtual void process_image(std::vector<uint8_t>& in, std::vector<uint8_t>& out) const = 0;

    // Nur Simulation: aufgelegte Masse in Gramm vorgeben (nullopt = automatisches Profil).
    virtual bool set_sim_load(std::optional<double> /*grams*/) { return false; }
};

} // namespace ew
