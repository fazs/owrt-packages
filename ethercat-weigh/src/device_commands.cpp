#include "device_commands.hpp"

#include <array>

#include "device_description.hpp"
#include "el3356.hpp"

namespace ew {

namespace {

constexpr int kHexBase = 16;          ///< Basis der Hex-Texte der Gerätebeschreibung
constexpr size_t kHexPerByte = 2U;    ///< Hex-Ziffern je Byte
constexpr size_t kSdoReadSize = 8U;   ///< Puffer für SDO-Lesezugriffe (größter Typ: 64 Bit)

/// Diagnoseeinträge (Doku Kap. 4.10.6): mV/V, Diag-Bits, ADC-Rohwerte der AI-Kanäle
constexpr std::array<const char*, 10> kDiagKeys = {
    "0x9000:11", "0xA000:11", "0xA000:12", "0xA000:13", "0xA000:15",
    "0xA000:16", "0xA000:17", "0xA000:18", "0x801E:01", "0x802E:01"};

/**
 * @brief Adresse "0xIIII:SS" zerlegen.
 * @param key Adresse
 * @param[out] index Objektindex
 * @param[out] sub Subindex
 */
void parse_key(const std::string& key, uint16_t& index, uint8_t& sub) {
    const auto colon = key.find(':');
    index = static_cast<uint16_t>(std::stoul(key.substr(0, colon), nullptr, kHexBase));
    sub = static_cast<uint8_t>(std::stoul(key.substr(colon + 1U), nullptr, kHexBase));
}

/**
 * @brief Hex-Text der Gerätebeschreibung als Bytes.
 * @param hex z. B. "0200"
 * @return Bytes in Textreihenfolge
 */
std::vector<uint8_t> hex_bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0U; i + kHexPerByte <= hex.size(); i += kHexPerByte)
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, kHexPerByte), nullptr, kHexBase)));
    return out;
}

/**
 * @brief Setzt das Request-Objekt 0xFB00:01 auf kCmdNone zurück.
 * @param io Backend
 */
void reset_request(IoBackend& io) {
    std::string unused;
    io.sdo_write(el3356::kObjCommand, el3356::kSubCmdRequest, el3356::le_u16(el3356::kCmdNone), unused);
}

} // namespace

/**
 * @brief Startet ein Kommando der Gerätebeschreibung.
 *
 * Schreibt alle schreibbaren Einträge des Kommandos (Einträge mit Zugriff "ro", z. B. Status
 * und Response im CmdObjectButton der ESI, entfallen).
 *
 * @param id Kennung laut Gerätebeschreibung (commands[].id)
 * @param io Backend
 * @param t aktuelle Zeit [s]
 * @param[out] finished true, wenn das Kommando ohne Statusabfrage abgeschlossen ist
 * @return Fehler bei unbekanntem Kommando, laufendem Kommando oder SDO-Fehler
 */
CalResult DeviceCommandRunner::start(const std::string& id, IoBackend& io, double t, bool& finished) {
    finished = false;
    if (active()) return {false, "Kommando " + id_ + " läuft noch"};
    const auto& d = DeviceDescription::el3356_0010();
    const nlohmann::json* cmd = nullptr;
    for (const auto& c : d.json().at("commands"))
        if (c.at("id") == id) cmd = &c;
    if (cmd == nullptr) return {false, "Kommando unbekannt: " + id};
    bool poll = false;
    for (const auto& w : cmd->at("writes")) {
        uint16_t index = 0U;
        uint8_t sub = 0U;
        parse_key(w.at("obj").get<std::string>(), index, sub);
        const auto* e = d.entry(index, sub);
        if (e == nullptr || e->raw.value("access", "") == "ro") continue;
        std::string err;
        if (!io.sdo_write(index, sub, hex_bytes(w.at("data").get<std::string>()), err)) {
            if (poll) reset_request(io);
            return {false, err};
        }
        poll = poll || (index == el3356::kObjCommand && sub == el3356::kSubCmdRequest);
    }
    if (!poll) {
        finished = true;
        return {true, {}};
    }
    id_ = id;
    started_ = t;
    return {true, {}};
}

/**
 * @brief Fragt den Status 0xFB00:02 des laufenden Kommandos ab.
 * @param io Backend
 * @param t aktuelle Zeit [s]
 * @return Ergebnis, sobald das Kommando abgeschlossen, fehlgeschlagen oder abgelaufen ist
 */
std::optional<CalResult> DeviceCommandRunner::step(IoBackend& io, double t) {
    if (!active()) return std::nullopt;
    std::vector<uint8_t> d;
    std::string err;
    CalResult r;
    if (!io.sdo_read(el3356::kObjCommand, el3356::kSubCmdStatus, sizeof(uint8_t), d, err) || d.empty()) {
        r = {false, err.empty() ? "Status 0xFB00:02 nicht lesbar" : err};
    } else if (d[0] == el3356::kCmdStatusBusy) {
        if (t - started_ <= kTimeoutS) return std::nullopt;
        r = {false, "Zeitüberschreitung"};
    } else if (d[0] == el3356::kCmdStatusDone) {
        r = {true, {}};
    } else {
        r = {false, "Status 0xFB00:02 = " + std::to_string(d[0])};
    }
    reset_request(io);
    id_.clear();
    return r;
}

/**
 * @brief Liest die Diagnoseeinträge der Klemme per SDO (nur auf Anfrage).
 * @param io Backend
 * @return {"ok": true, "items": [{"key", "name", "description", "type", "value", "error"}]}
 */
nlohmann::json read_diagnostics(IoBackend& io) {
    const auto& d = DeviceDescription::el3356_0010();
    nlohmann::json items = nlohmann::json::array();
    for (const char* key : kDiagKeys) {
        uint16_t index = 0U;
        uint8_t sub = 0U;
        parse_key(key, index, sub);
        const auto* e = d.entry(index, sub);
        nlohmann::json it = {{"key", key}, {"value", nullptr}, {"error", ""}};
        if (e == nullptr) {
            it["error"] = "nicht in der Gerätebeschreibung";
            items.push_back(it);
            continue;
        }
        el3356::DType t = el3356::DType::None;
        el3356::dtype_from_name(e->type, t);
        it["name"] = e->name;
        it["description"] = e->raw.value("description_de", "");
        it["type"] = e->type;
        std::vector<uint8_t> data;
        std::string err;
        if (io.sdo_read(index, sub, kSdoReadSize, data, err)) {
            const double v = el3356::decode_value(t, data);
            it["value"] = t == el3356::DType::Bool ? nlohmann::json(v != 0.0) : nlohmann::json(v);
        } else {
            it["error"] = err;
        }
        items.push_back(it);
    }
    return {{"ok", true}, {"items", items}};
}

} // namespace ew
