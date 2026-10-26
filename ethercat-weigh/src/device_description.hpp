#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ew {

/// Eintrag des Objektverzeichnisses laut Gerätebeschreibung.
struct DescEntry {
    uint16_t index = 0;               ///< Objektindex
    uint8_t sub = 0;                  ///< Subindex
    std::string name;                 ///< Name laut ESI
    std::string type;                 ///< Kurzname (dtype_name()), "string" oder "bytes"
    uint16_t bits = 0;                ///< Länge in Bit
    std::vector<uint32_t> revisions;  ///< Revisionen mit diesem Eintrag; leer = alle
    nlohmann::json raw;               ///< vollständiger Eintrag (Default, Grenzen, Labels, …)
};

/// Eintrag eines PDO-Mappings laut Gerätebeschreibung.
struct DescPdoEntry {
    uint16_t index = 0;  ///< Objektindex, 0 = Lücke
    uint8_t sub = 0;     ///< Subindex
    uint8_t bits = 0;    ///< Länge in Bit
    std::string name;    ///< Name im Mapping (ESI, z. B. "Status__Overrange"), leer bei Lücken
    std::string type;    ///< Kurzname, leer bei Lücken
};

/// Mapping eines PDOs für eine Gruppe von Revisionen.
struct DescPdoLayout {
    std::vector<uint32_t> revisions;     ///< leer = alle Revisionen des PDOs
    std::vector<DescPdoEntry> entries;   ///< Einträge in Mapping-Reihenfolge
};

/// RxPDO bzw. TxPDO laut Gerätebeschreibung.
struct DescPdo {
    uint16_t index = 0;                  ///< 0x16xx bzw. 0x1Axx
    std::string name;                    ///< Name laut ESI
    bool rx = false;                     ///< RxPDO (Ausgänge)
    int sm = -1;                         ///< Sync-Manager laut ESI, -1 = ohne Angabe
    bool fixed = false;                  ///< Mapping nicht änderbar
    std::vector<uint16_t> exclude;       ///< nicht gemeinsam zuordenbar
    std::vector<uint32_t> revisions;     ///< Revisionen mit diesem PDO; leer = alle
    std::vector<DescPdoLayout> layouts;  ///< Mapping je Revisionsgruppe
};

/**
 * @brief Aus der ESI erzeugte Gerätebeschreibung (src/generated/el3356_0010.json, scripts/esi_gen.py).
 *
 * Hält das JSON und daraus typisierte Objekteinträge und PDOs. Revision 0 bei Abfragen
 * bedeutet die neueste Revision der Beschreibung.
 */
class DeviceDescription {
public:
    explicit DeviceDescription(std::string_view text);

    static const DeviceDescription& el3356_0010();

    [[nodiscard]] const nlohmann::json& json() const { return json_; }      ///< vollständige Beschreibung
    [[nodiscard]] const std::string& text() const { return text_; }         ///< JSON-Text wie eingebettet
    [[nodiscard]] uint32_t newest_revision() const { return revisions_.back(); }  ///< neueste Revision
    [[nodiscard]] const std::vector<uint32_t>& revisions() const { return revisions_; }  ///< alle Revisionen
    [[nodiscard]] const std::vector<DescEntry>& entries() const { return entries_; }     ///< alle Einträge
    [[nodiscard]] const std::vector<DescPdo>& pdos() const { return pdos_; }             ///< alle PDOs

    [[nodiscard]] const DescEntry* entry(uint16_t index, uint8_t sub) const;
    [[nodiscard]] const DescPdo* pdo(uint16_t index) const;
    [[nodiscard]] const DescPdoLayout* layout(const DescPdo& p, uint32_t revision = 0U) const;
    [[nodiscard]] bool in_revision(const std::vector<uint32_t>& revisions, uint32_t revision = 0U) const;

private:
    std::string text_;                 ///< JSON-Text
    nlohmann::json json_;              ///< geparste Beschreibung
    std::vector<uint32_t> revisions_;  ///< Revisionen, aufsteigend
    std::vector<DescEntry> entries_;   ///< Einträge nach Index, Subindex
    std::vector<DescPdo> pdos_;        ///< PDOs nach Index
};

} // namespace ew
