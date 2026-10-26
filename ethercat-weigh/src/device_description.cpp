#include "device_description.hpp"

#include <algorithm>
#include <stdexcept>

namespace ew {

/// JSON-Text der Beschreibung der EL3356-0010 (von CMake aus src/generated/el3356_0010.json erzeugt).
extern const std::string_view kEl3356_0010Json;

namespace {

constexpr int kHexBase = 16;  ///< Basis der Adressen "0xIIII", "0xSS" und Revisionen

/**
 * @brief Zahl aus "0x…"-Text der Beschreibung.
 * @param j JSON-String
 * @return Wert
 * @throws std::invalid_argument bei ungültigem Text
 */
uint32_t hex(const nlohmann::json& j) {
    return static_cast<uint32_t>(std::stoul(j.get<std::string>(), nullptr, kHexBase));
}

/**
 * @brief Revisionsliste eines Elements der Beschreibung.
 * @param j Element mit optionalem Feld "revisions"
 * @return Revisionen, leer = alle
 */
std::vector<uint32_t> revisions_of(const nlohmann::json& j) {
    std::vector<uint32_t> out;
    if (j.contains("revisions"))
        for (const auto& r : j.at("revisions")) out.push_back(hex(r));
    return out;
}

} // namespace

/**
 * @brief Parst eine Gerätebeschreibung.
 * @param text JSON-Text (Ausgabe von scripts/esi_gen.py)
 * @throws nlohmann::json::exception, std::invalid_argument bei ungültigem Inhalt
 */
DeviceDescription::DeviceDescription(std::string_view text) : text_(text), json_(nlohmann::json::parse(text_)) {
    for (const auto& r : json_.at("device").at("revisions")) revisions_.push_back(hex(r));
    if (revisions_.empty()) throw std::invalid_argument("Gerätebeschreibung ohne Revision");
    std::sort(revisions_.begin(), revisions_.end());

    for (const auto& o : json_.at("objects")) {
        const auto index = static_cast<uint16_t>(hex(o.at("index")));
        const auto obj_revs = revisions_of(o);
        for (const auto& e : o.at("entries")) {
            DescEntry d;
            d.index = index;
            d.sub = static_cast<uint8_t>(hex(e.at("sub")));
            d.name = e.at("name").get<std::string>();
            d.type = e.at("type").get<std::string>();
            d.bits = e.at("bits").get<uint16_t>();
            d.revisions = e.contains("revisions") ? revisions_of(e) : obj_revs;
            d.raw = e;
            entries_.push_back(std::move(d));
        }
    }
    for (const auto& p : json_.at("pdos")) {
        DescPdo d;
        d.index = static_cast<uint16_t>(hex(p.at("index")));
        d.name = p.at("name").get<std::string>();
        d.rx = p.at("dir").get<std::string>() == "rx";
        d.sm = p.value("sm", -1);
        d.fixed = p.value("fixed", false);
        for (const auto& x : p.at("exclude")) d.exclude.push_back(static_cast<uint16_t>(hex(x)));
        d.revisions = revisions_of(p);
        for (const auto& l : p.at("layouts")) {
            DescPdoLayout lay;
            lay.revisions = revisions_of(l);
            for (const auto& e : l.at("entries")) {
                DescPdoEntry pe;
                pe.index = static_cast<uint16_t>(hex(e.at("index")));
                pe.bits = e.at("bits").get<uint8_t>();
                if (pe.index != 0U) {
                    pe.sub = static_cast<uint8_t>(hex(e.at("sub")));
                    pe.name = e.at("name").get<std::string>();
                    pe.type = e.value("type", "");
                }
                lay.entries.push_back(std::move(pe));
            }
            d.layouts.push_back(std::move(lay));
        }
        pdos_.push_back(std::move(d));
    }
}

/**
 * @brief Eingebettete Beschreibung der EL3356-0010.
 * @return Beschreibung; beim ersten Aufruf geparst
 */
const DeviceDescription& DeviceDescription::el3356_0010() {
    static const DeviceDescription d(kEl3356_0010Json);
    return d;
}

/**
 * @brief Prüft, ob eine Revision in einer Revisionsliste enthalten ist.
 * @param revisions Liste aus der Beschreibung, leer = alle
 * @param revision Revision, 0 = neueste
 * @return true, wenn enthalten
 */
bool DeviceDescription::in_revision(const std::vector<uint32_t>& revisions, uint32_t revision) const {
    if (revisions.empty()) return true;
    const uint32_t r = revision != 0U ? revision : newest_revision();
    return std::find(revisions.begin(), revisions.end(), r) != revisions.end();
}

/**
 * @brief Eintrag des Objektverzeichnisses.
 * @param index Objektindex
 * @param sub Subindex
 * @return Eintrag oder nullptr
 */
const DescEntry* DeviceDescription::entry(uint16_t index, uint8_t sub) const {
    for (const auto& e : entries_)
        if (e.index == index && e.sub == sub) return &e;
    return nullptr;
}

/**
 * @brief PDO nach Index.
 * @param index 0x16xx bzw. 0x1Axx
 * @return PDO oder nullptr
 */
const DescPdo* DeviceDescription::pdo(uint16_t index) const {
    for (const auto& p : pdos_)
        if (p.index == index) return &p;
    return nullptr;
}

/**
 * @brief Mapping eines PDOs in einer Revision.
 * @param p PDO
 * @param revision Revision, 0 = neueste
 * @return Mapping oder nullptr, wenn das PDO in der Revision fehlt
 */
const DescPdoLayout* DeviceDescription::layout(const DescPdo& p, uint32_t revision) const {
    if (!in_revision(p.revisions, revision)) return nullptr;
    for (const auto& l : p.layouts)
        if (in_revision(l.revisions, revision)) return &l;
    return nullptr;
}

} // namespace ew
