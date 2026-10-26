#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ew {

/// Entry of the object dictionary according to the device description.
struct DescEntry {
    uint16_t index = 0;               ///< object index
    uint8_t sub = 0;                  ///< subindex
    std::string name;                 ///< name according to the ESI
    std::string type;                 ///< short name (dtype_name()), "string" or "bytes"
    uint16_t bits = 0;                ///< length in bits
    std::vector<uint32_t> revisions;  ///< revisions with this entry; empty = all
    nlohmann::json raw;               ///< complete entry (default, limits, labels, …)
};

/// Entry of a PDO mapping according to the device description.
struct DescPdoEntry {
    uint16_t index = 0;  ///< object index, 0 = gap
    uint8_t sub = 0;     ///< subindex
    uint8_t bits = 0;    ///< length in bits
    std::string name;    ///< name in the mapping (ESI, e.g. "Status__Overrange"), empty for gaps
    std::string type;    ///< short name, empty for gaps
};

/// Mapping of a PDO for a group of revisions.
struct DescPdoLayout {
    std::vector<uint32_t> revisions;     ///< empty = all revisions of the PDO
    std::vector<DescPdoEntry> entries;   ///< entries in mapping order
};

/// RxPDO or TxPDO according to the device description.
struct DescPdo {
    uint16_t index = 0;                  ///< 0x16xx or 0x1Axx
    std::string name;                    ///< name according to the ESI
    bool rx = false;                     ///< RxPDO (outputs)
    int sm = -1;                         ///< sync manager according to the ESI, -1 = not specified
    bool fixed = false;                  ///< mapping cannot be changed
    std::vector<uint16_t> exclude;       ///< cannot be assigned together
    std::vector<uint32_t> revisions;     ///< revisions with this PDO; empty = all
    std::vector<DescPdoLayout> layouts;  ///< mapping per revision group
};

/**
 * @brief Device description generated from the ESI (src/generated/el3356_0010.json, scripts/esi_gen.py).
 *
 * Holds the JSON and the typed object entries and PDOs derived from it. Revision 0 in queries
 * means the latest revision of the description.
 */
class DeviceDescription {
public:
    explicit DeviceDescription(std::string_view text);

    static const DeviceDescription& el3356_0010();

    [[nodiscard]] const nlohmann::json& json() const { return json_; }      ///< complete description
    [[nodiscard]] const std::string& text() const { return text_; }         ///< JSON text as embedded
    [[nodiscard]] uint32_t newest_revision() const { return revisions_.back(); }  ///< latest revision
    [[nodiscard]] const std::vector<uint32_t>& revisions() const { return revisions_; }  ///< all revisions
    [[nodiscard]] const std::vector<DescEntry>& entries() const { return entries_; }     ///< all entries
    [[nodiscard]] const std::vector<DescPdo>& pdos() const { return pdos_; }             ///< all PDOs

    [[nodiscard]] const DescEntry* entry(uint16_t index, uint8_t sub) const;
    [[nodiscard]] const DescPdo* pdo(uint16_t index) const;
    [[nodiscard]] const DescPdoLayout* layout(const DescPdo& p, uint32_t revision = 0U) const;
    [[nodiscard]] bool in_revision(const std::vector<uint32_t>& revisions, uint32_t revision = 0U) const;

private:
    std::string text_;                 ///< JSON text
    nlohmann::json json_;              ///< parsed description
    std::vector<uint32_t> revisions_;  ///< revisions, ascending
    std::vector<DescEntry> entries_;   ///< entries by index, subindex
    std::vector<DescPdo> pdos_;        ///< PDOs by index
};

} // namespace ew
