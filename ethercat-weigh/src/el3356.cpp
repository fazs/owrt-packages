#include "el3356.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ew::el3356 {

/**
 * @brief Zerlegt einen Mapping-Eintrag aus 0x16xx/0x1Axx.
 * @param raw UINT32: index<<16 | sub<<8 | bitlen
 * @return Index, Subindex, Bitlänge
 */
MapEntry decode_map_entry(uint32_t raw) {
    return {static_cast<uint16_t>(raw >> 16), static_cast<uint8_t>((raw >> 8) & 0xFF),
            static_cast<uint8_t>(raw & 0xFF)};
}

namespace {

/**
 * @brief Durchläuft alle Einträge der PDOs in Zuordnungsreihenfolge.
 * @param pdos Mappings je PDO
 * @param on_entry Aufruf (Eintrag, Bitoffset ab Beginn des Abbilds)
 */
template <typename F>
void walk(const std::vector<std::vector<MapEntry>>& pdos, F&& on_entry) {
    uint32_t off = 0;
    for (const auto& pdo : pdos)
        for (const auto& e : pdo) {
            on_entry(e, off);
            off += e.bits;
        }
}

/**
 * @brief Liest ein Bitfeld (little endian, bis 64 Bit).
 * @param img Prozessabbild
 * @param off Bitoffset
 * @param len Länge in Bit
 * @return Rohwert
 */
uint64_t get_bits(const uint8_t* img, uint32_t off, uint8_t len) {
    uint64_t v = 0;
    for (uint8_t i = 0; i < len && i < 64; ++i, ++off)
        v |= static_cast<uint64_t>((img[off / 8] >> (off % 8)) & 1u) << i;
    return v;
}

/**
 * @brief Schreibt ein Bitfeld (little endian, bis 64 Bit).
 * @param img Prozessabbild
 * @param off Bitoffset
 * @param len Länge in Bit
 * @param v Rohwert
 */
void set_bits(uint8_t* img, uint32_t off, uint8_t len, uint64_t v) {
    for (uint8_t i = 0; i < len && i < 64; ++i, ++off) {
        const uint8_t mask = static_cast<uint8_t>(1u << (off % 8));
        if ((v >> i) & 1u)
            img[off / 8] |= mask;
        else
            img[off / 8] &= static_cast<uint8_t>(~mask);
    }
}

/// @brief Liest ein Bit; false, wenn der Eintrag nicht gemappt ist.
bool get_bit(const uint8_t* img, const EntryLoc& l) {
    if (!l.present) return false;
    return (img[l.bit_offset / 8] >> (l.bit_offset % 8)) & 1u;
}

/// @brief Setzt oder löscht ein Bit; ohne Wirkung, wenn der Eintrag nicht gemappt ist.
void set_bit(uint8_t* img, const EntryLoc& l, bool v) {
    if (!l.present) return;
    const uint8_t mask = static_cast<uint8_t>(1u << (l.bit_offset % 8));
    if (v)
        img[l.bit_offset / 8] |= mask;
    else
        img[l.bit_offset / 8] &= static_cast<uint8_t>(~mask);
}

/// @brief Bytes eines Werts in Speicherreihenfolge (Zielplattformen sind little endian).
template <typename T>
std::vector<uint8_t> le(T v) {
    std::vector<uint8_t> out(sizeof(T));
    std::memcpy(out.data(), &v, sizeof(T));  // Zielplattformen sind little endian
    return out;
}

/**
 * @brief Rohwert (bis 64 Bit, little endian) als Zahl gemäß Typ.
 * @param t Datentyp
 * @param raw Rohbits
 * @return Wert; U64 als double gerundet
 */
double from_raw(DType t, uint64_t raw) {
    switch (t) {
    case DType::Bool: return raw & 1u;
    case DType::I8: return static_cast<int8_t>(raw);
    case DType::I16: return static_cast<int16_t>(raw);
    case DType::I32: return static_cast<int32_t>(raw);
    case DType::F32: {
        const auto r32 = static_cast<uint32_t>(raw);
        float f;
        std::memcpy(&f, &r32, sizeof f);
        return f;
    }
    default: return static_cast<double>(raw);
    }
}

/**
 * @brief Zahl als Rohwert gemäß Typ (Ganzzahlen gerundet, vorzeichenlose ab 0).
 * @param t Datentyp
 * @param v Wert
 * @return Rohbits
 */
uint64_t to_raw(DType t, double v) {
    switch (t) {
    case DType::Bool: return v != 0.0 ? 1u : 0u;
    case DType::I8:
    case DType::I16:
    case DType::I32: return static_cast<uint32_t>(static_cast<int32_t>(std::lround(v)));
    case DType::U64: return static_cast<uint64_t>(v < 0.0 ? 0.0 : v);
    case DType::F32: {
        const float f = static_cast<float>(v);
        uint32_t raw;
        std::memcpy(&raw, &f, sizeof raw);
        return raw;
    }
    default: return static_cast<uint32_t>(std::llround(v < 0.0 ? 0.0 : v));
    }
}

/// Bekannter Objekteintrag laut ESI.
struct KnownEntry {
    uint16_t index;    ///< Objektindex
    uint8_t sub;       ///< Subindex
    const char* name;  ///< Name laut ESI
    DType type;        ///< Datentyp
};

/// Objektverzeichnis laut ESI (Revision 0x0019): RMB 0x6000/0x7000, AI-Kanäle 0x6010/0x6020
const KnownEntry kKnownEntries[] = {
    {kObjInputs, 0x02, "Overrange", DType::Bool},
    {kObjInputs, 0x04, "Data invalid", DType::Bool},
    {kObjInputs, 0x07, "Error", DType::Bool},
    {kObjInputs, 0x08, "Calibration in progress", DType::Bool},
    {kObjInputs, 0x09, "Steady state", DType::Bool},
    {kObjInputs, 0x0E, "Sync error", DType::Bool},
    {kObjInputs, 0x10, "TxPDO Toggle", DType::Bool},
    {kObjInputs, 0x11, "Value", DType::I32},
    {kObjInputs, 0x12, "Value (Real)", DType::F32},
    {kObjInputs, 0x13, "Timestamp", DType::U64},
    {0x6010, 0x01, "Underrange", DType::Bool},
    {0x6010, 0x02, "Overrange", DType::Bool},
    {0x6010, 0x07, "Error", DType::Bool},
    {0x6010, 0x10, "TxPDO Toggle", DType::Bool},
    {0x6010, 0x11, "Value", DType::I32},
    {0x6020, 0x01, "Underrange", DType::Bool},
    {0x6020, 0x02, "Overrange", DType::Bool},
    {0x6020, 0x07, "Error", DType::Bool},
    {0x6020, 0x10, "TxPDO Toggle", DType::Bool},
    {0x6020, 0x11, "Value", DType::I32},
    {kObjSmOutParam, 0x20, "Sync error", DType::Bool},
    {kObjTxPdoPar, 0x09, "TxPDO Toggle", DType::Bool},
    {kObjControl, 0x01, "Start calibration", DType::Bool},
    {kObjControl, 0x02, "Disable calibration", DType::Bool},
    {kObjControl, 0x03, "Input freeze", DType::Bool},
    {kObjControl, 0x04, "Sample mode", DType::Bool},
    {kObjControl, 0x05, "Tara", DType::Bool},
    {kObjControl, 0x11, "Filter frequency", DType::U16},
};

/**
 * @brief PDO-Eintrag mit Name und Typ aus der ESI-Tabelle.
 * @param index Objektindex (0 = Lücke)
 * @param sub Subindex
 * @param bits Länge in Bit
 * @return Eintrag; unbekannte Objekte heißen "0xIIII:SS", Typ nach Bitlänge
 */
PdoEntry known(uint16_t index, uint8_t sub, uint8_t bits) {
    PdoEntry e;
    e.map = {index, sub, bits};
    if (!known_entry(index, sub, e.name, e.type) && index != 0) {
        e.type = dtype_from_coe(0, bits);
        char b[16];
        std::snprintf(b, sizeof b, "0x%04X:%02X", index, sub);
        e.name = b;
    }
    return e;
}

} // namespace

/**
 * @brief Kurzname eines Datentyps für JSON.
 * @param t Datentyp
 * @return "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "f32" bzw. "" für None
 */
const char* dtype_name(DType t) {
    switch (t) {
    case DType::Bool: return "bool";
    case DType::U8: return "u8";
    case DType::U16: return "u16";
    case DType::U32: return "u32";
    case DType::U64: return "u64";
    case DType::I8: return "i8";
    case DType::I16: return "i16";
    case DType::I32: return "i32";
    case DType::F32: return "f32";
    default: return "";
    }
}

/**
 * @brief Datentyp aus seinem Kurznamen.
 * @param s Kurzname (siehe dtype_name())
 * @param[out] t Datentyp
 * @return false bei unbekanntem Namen
 */
bool dtype_from_name(const std::string& s, DType& t) {
    for (DType c : {DType::Bool, DType::U8, DType::U16, DType::U32, DType::U64, DType::I8, DType::I16,
                    DType::I32, DType::F32})
        if (s == dtype_name(c)) {
            t = c;
            return true;
        }
    return false;
}

/**
 * @brief Datentyp aus dem CoE-Datentypcode (ETG.1000.6).
 * @param coe ECT_*-Code aus der SDO-Information, 0 = unbekannt
 * @param bits Bitlänge, Rückfall bei unbekanntem Code
 * @return Datentyp
 */
DType dtype_from_coe(uint16_t coe, uint8_t bits) {
    switch (coe) {  // ECT_BOOLEAN .. ECT_REAL32 (ETG.1000.6)
    case 0x0001: return DType::Bool;
    case 0x0002: return DType::I8;
    case 0x0003: return DType::I16;
    case 0x0004: return DType::I32;
    case 0x0005: return DType::U8;
    case 0x0006: return DType::U16;
    case 0x0007: return DType::U32;
    case 0x0008: return DType::F32;
    case 0x001B: return DType::U64;
    default: break;
    }
    if (bits == 1) return DType::Bool;
    if (bits <= 8) return DType::U8;
    if (bits <= 16) return DType::U16;
    if (bits <= 32) return DType::U32;
    return DType::U64;
}

/**
 * @brief Größe für SDO-Zugriffe.
 * @param t Datentyp
 * @return Bytes, 0 für None
 */
size_t dtype_size(DType t) {
    switch (t) {
    case DType::Bool:
    case DType::U8:
    case DType::I8: return 1;
    case DType::U16:
    case DType::I16: return 2;
    case DType::U32:
    case DType::I32:
    case DType::F32: return 4;
    case DType::U64: return 8;
    default: return 0;
    }
}

/**
 * @brief Kodiert einen Wert für einen SDO-Schreibzugriff.
 * @param t Datentyp
 * @param v Wert
 * @return dtype_size(t) Bytes, little endian
 */
std::vector<uint8_t> encode_value(DType t, double v) {
    const uint64_t raw = to_raw(t, v);
    std::vector<uint8_t> out(dtype_size(t));
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(raw >> (8 * i));
    return out;
}

/**
 * @brief Dekodiert SDO-Daten.
 * @param t Datentyp
 * @param d Daten, little endian; überzählige Bytes werden ignoriert
 * @return Wert
 */
double decode_value(DType t, const std::vector<uint8_t>& d) {
    uint64_t raw = 0;
    const size_t n = dtype_size(t) ? dtype_size(t) : 8;
    for (size_t i = 0; i < d.size() && i < n; ++i) raw |= static_cast<uint64_t>(d[i]) << (8 * i);
    return from_raw(t, raw);
}

/**
 * @brief Berechnet die Lage der ausgewerteten Eingänge.
 *
 * Erkennt Sync error und TxPDO Toggle sowohl in 0x6000 als auch (bis Revision 0x0014)
 * in 0x1C32:20 bzw. 0x1800:09.
 *
 * @param pdos Mappings der TxPDOs in Reihenfolge von 0x1C13
 * @return Lage relativ zum Beginn des Eingangsabbilds
 */
InputLayout layout_inputs(const std::vector<std::vector<MapEntry>>& pdos) {
    InputLayout l;
    walk(pdos, [&](const MapEntry& e, uint32_t off) {
        EntryLoc loc{true, off, e.bits};
        // Bis Revision 0x0014 liegen Sync error und TxPDO Toggle in 0x1C32:20 bzw. 0x1800:09
        if (e.index == kObjSmOutParam && e.sub == 0x20) l.sync_error = loc;
        if (e.index == kObjTxPdoPar && e.sub == 0x09) l.toggle = loc;
        if (e.index != kObjInputs) return;
        switch (e.sub) {
        case 0x02: l.overrange = loc; break;
        case 0x04: l.data_invalid = loc; break;
        case 0x07: l.error = loc; break;
        case 0x08: l.calibration = loc; break;
        case 0x09: l.steady_state = loc; break;
        case 0x0E: l.sync_error = loc; break;
        case 0x10: l.toggle = loc; break;
        case 0x12: l.value_real = loc; break;
        default: break;
        }
    });
    return l;
}

/**
 * @brief Berechnet die Lage der Steuerbits 0x7000.
 * @param pdos Mappings der RxPDOs in Reihenfolge von 0x1C12
 * @return Lage relativ zum Beginn des Ausgangsabbilds
 */
OutputLayout layout_outputs(const std::vector<std::vector<MapEntry>>& pdos) {
    OutputLayout l;
    walk(pdos, [&](const MapEntry& e, uint32_t off) {
        if (e.index != kObjControl) return;
        EntryLoc loc{true, off, e.bits};
        switch (e.sub) {
        case 0x01: l.start_calibration = loc; break;
        case 0x02: l.disable_calibration = loc; break;
        case 0x03: l.input_freeze = loc; break;
        case 0x04: l.sample_mode = loc; break;
        case 0x05: l.tare = loc; break;
        default: break;
        }
    });
    return l;
}

/**
 * @brief Liest die Statusbits aus dem Eingangsabbild.
 * @param img Eingangsabbild des Slaves
 * @param l Lage der Einträge
 * @return Statusbits; nicht gemappte Bits sind false
 */
El3356Status decode_status(const uint8_t* img, const InputLayout& l) {
    El3356Status s;
    s.overrange = get_bit(img, l.overrange);
    s.data_invalid = get_bit(img, l.data_invalid);
    s.error = get_bit(img, l.error);
    s.calibration_in_progress = get_bit(img, l.calibration);
    s.steady_state = get_bit(img, l.steady_state);
    s.sync_error = get_bit(img, l.sync_error);
    s.txpdo_toggle = get_bit(img, l.toggle);
    return s;
}

/**
 * @brief Liest Value (Real) 0x6000:12 aus dem Eingangsabbild.
 * @param img Eingangsabbild des Slaves
 * @param l Lage der Einträge
 * @return Wert, 0 wenn nicht byteweise mit 32 Bit gemappt
 */
float decode_value_real(const uint8_t* img, const InputLayout& l) {
    float f = 0.0f;
    if (!l.value_real.present || l.value_real.bit_len != 32 || (l.value_real.bit_offset % 8) != 0)
        return f;
    std::memcpy(&f, img + l.value_real.bit_offset / 8, sizeof(f));
    return f;
}

/**
 * @brief Schreibt die Steuerbits in das Ausgangsabbild.
 * @param img Ausgangsabbild des Slaves
 * @param l Lage der Steuerbits
 * @param c Steuerbits
 */
void encode_control(uint8_t* img, const OutputLayout& l, const El3356Control& c) {
    set_bit(img, l.start_calibration, c.start_calibration);
    set_bit(img, l.disable_calibration, c.disable_calibration);
    set_bit(img, l.input_freeze, c.input_freeze);
    set_bit(img, l.sample_mode, c.sample_mode);
    set_bit(img, l.tare, c.tare);
}

/**
 * @brief Schreibt Statusbits und Value (Real) in ein Eingangsabbild (Simulation).
 * @param img Eingangsabbild
 * @param l Lage der Einträge
 * @param s Statusbits
 * @param value_real Value (Real)
 */
void encode_inputs(uint8_t* img, const InputLayout& l, const El3356Status& s, float value_real) {
    set_bit(img, l.overrange, s.overrange);
    set_bit(img, l.data_invalid, s.data_invalid);
    set_bit(img, l.error, s.error);
    set_bit(img, l.calibration, s.calibration_in_progress);
    set_bit(img, l.steady_state, s.steady_state);
    set_bit(img, l.sync_error, s.sync_error);
    set_bit(img, l.toggle, s.txpdo_toggle);
    if (l.value_real.present && l.value_real.bit_len == 32)
        set_bits(img, l.value_real.bit_offset, 32, to_raw(DType::F32, value_real));
}

/**
 * @brief Zugriff auf das Steuerbit 0x7000:@p sub.
 * @param c Steuerbits
 * @param sub Subindex 1..5
 * @return Zeiger auf das Feld in @p c, nullptr bei unbekanntem Subindex
 */
bool* control_bit(El3356Control& c, uint8_t sub) {
    switch (sub) {
    case 0x01: return &c.start_calibration;
    case 0x02: return &c.disable_calibration;
    case 0x03: return &c.input_freeze;
    case 0x04: return &c.sample_mode;
    case 0x05: return &c.tare;
    default: return nullptr;
    }
}

/**
 * @brief Name und Typ eines Eintrags laut ESI.
 * @param index Objektindex
 * @param sub Subindex
 * @param[out] name Name
 * @param[out] type Datentyp
 * @return false, wenn der Eintrag unbekannt ist
 */
bool known_entry(uint16_t index, uint8_t sub, std::string& name, DType& type) {
    for (const auto& k : kKnownEntries)
        if (k.index == index && k.sub == sub) {
            name = k.name;
            type = k.type;
            return true;
        }
    return false;
}

/**
 * @brief Name eines PDOs laut ESI.
 * @param pdo Index 0x16xx/0x1Axx
 * @return Name, sonst "0xIIII"
 */
std::string known_pdo_name(uint16_t pdo) {
    switch (pdo) {
    case kPdoControl: return "RMB Control";
    case 0x1601: return "RMB Filter frequency";
    case kPdoStatus: return "RMB Status";
    case 0x1A01: return "RMB Value (INT32)";
    case kPdoValueReal: return "RMB Value (Real)";
    case 0x1A03: return "RMB Timestamp";
    case 0x1A04: return "AI Standard Channel 1";
    case 0x1A05: return "AI Compact Channel 1";
    case 0x1A06: return "AI Standard Channel 2";
    case 0x1A07: return "AI Compact Channel 2";
    default: {
        char b[8];
        std::snprintf(b, sizeof b, "0x%04X", pdo);
        return b;
    }
    }
}

/**
 * @brief PDOs der EL3356-0010 laut ESI (Revision 0x0019) mit Mapping und Exclude-Listen.
 *
 * Grundlage für die Simulation und Rückfall für Namen ohne SDO-Information.
 *
 * @return alle PDOs, nicht zugeordnet
 */
std::vector<PdoInfo> known_pdos() {
    auto pdo = [](uint16_t idx, bool rx, std::vector<PdoEntry> e, std::vector<uint16_t> ex) {
        return PdoInfo{idx, known_pdo_name(idx), rx, false, std::move(e), std::move(ex)};
    };
    auto ai_standard = [](uint16_t obj) {
        return std::vector<PdoEntry>{known(obj, 0x01, 1), known(obj, 0x02, 1), known(0, 0, 4),
                                     known(obj, 0x07, 1), known(0, 0, 1), known(0, 0, 7),
                                     known(obj, 0x10, 1), known(obj, 0x11, 32)};
    };
    const std::vector<uint16_t> ai = {0x1A04, 0x1A05, 0x1A06, 0x1A07};
    return {
        pdo(kPdoControl, true,
            {known(kObjControl, 0x01, 1), known(kObjControl, 0x02, 1), known(kObjControl, 0x03, 1),
             known(kObjControl, 0x04, 1), known(kObjControl, 0x05, 1), known(0, 0, 3),
             known(0, 0, 8)},
            {}),
        pdo(0x1601, true, {known(kObjControl, 0x11, 16)}, {}),
        pdo(kPdoStatus, false,
            {known(0, 0, 1), known(kObjInputs, 0x02, 1), known(0, 0, 1), known(kObjInputs, 0x04, 1),
             known(0, 0, 2), known(kObjInputs, 0x07, 1), known(kObjInputs, 0x08, 1),
             known(kObjInputs, 0x09, 1), known(0, 0, 4), known(kObjInputs, 0x0E, 1), known(0, 0, 1),
             known(kObjInputs, 0x10, 1)},
            ai),
        pdo(0x1A01, false, {known(kObjInputs, 0x11, 32)},
            {kPdoValueReal, 0x1A04, 0x1A05, 0x1A06, 0x1A07}),
        pdo(kPdoValueReal, false, {known(kObjInputs, 0x12, 32)}, {0x1A01, 0x1A04, 0x1A05, 0x1A06, 0x1A07}),
        pdo(0x1A03, false, {known(kObjInputs, 0x13, 64)}, ai),
        pdo(0x1A04, false, ai_standard(0x6010), {0x1A05, 0x1A00, 0x1A01, 0x1A02, 0x1A03}),
        pdo(0x1A05, false, {known(0x6010, 0x11, 32)}, {0x1A04, 0x1A00, 0x1A01, 0x1A02, 0x1A03}),
        pdo(0x1A06, false, ai_standard(0x6020), {0x1A07, 0x1A00, 0x1A01, 0x1A02, 0x1A03}),
        pdo(0x1A07, false, {known(0x6020, 0x11, 32)}, {0x1A06, 0x1A00, 0x1A01, 0x1A02, 0x1A03}),
    };
}

/**
 * @brief Setzt assigned und die Lage der Einträge gemäß Zuordnungsreihenfolge.
 * @param pdos PDO-Katalog
 * @param rx_assign Inhalt von 0x1C12
 * @param tx_assign Inhalt von 0x1C13
 */
void locate_pdos(std::vector<PdoInfo>& pdos, const std::vector<uint16_t>& rx_assign,
                 const std::vector<uint16_t>& tx_assign) {
    for (auto& p : pdos) {
        p.assigned = false;
        for (auto& e : p.entries) e.loc = {};
    }
    for (bool rx : {true, false}) {
        uint32_t off = 0;
        for (uint16_t idx : rx ? rx_assign : tx_assign)
            for (auto& p : pdos) {
                if (p.index != idx || p.rx != rx) continue;
                p.assigned = true;
                for (auto& e : p.entries) {
                    e.loc = {true, off, e.map.bits};
                    off += e.map.bits;
                }
            }
    }
}

/**
 * @brief Wert eines zugeordneten Eintrags aus dem Abbild.
 * @param img Ein- bzw. Ausgangsabbild des Slaves
 * @param e Eintrag mit Lage
 * @return Wert (Bitfeld bis 64 Bit; U64 als double gerundet), 0 für Lücken
 */
double entry_value(const uint8_t* img, const PdoEntry& e) {
    if (!e.loc.present || e.type == DType::None) return 0.0;
    return from_raw(e.type, get_bits(img, e.loc.bit_offset, e.loc.bit_len));
}

/**
 * @brief Schreibliste für 0x8000 aus der Konfiguration.
 * @param dc Klemmenparameter
 * @return SDO-Schreibzugriffe (auch in OP zulässig)
 */
std::vector<SdoWrite> settings_writes(const DeviceConfig& dc) {
    auto w = [](uint8_t sub, DType t, double v, const char* name) {
        return SdoWrite{kObjSettings, sub, encode_value(t, v), name, t, false};
    };
    return {
        w(0x01, DType::Bool, dc.mode0_enable_filter, "Mode0 enable filter"),
        w(0x03, DType::Bool, dc.mode0_enable_averager, "Mode0 enable averager"),
        w(0x11, DType::U16, dc.mode0_filter_settings, "Mode0 filter settings"),
        w(0x23, DType::F32, dc.rated_output_mv_v, "Rated output"),
        w(0x24, DType::F32, dc.nominal_load, "Nominal load"),
        w(0x25, DType::F32, dc.zero_balance, "Zero balance"),
        w(0x26, DType::F32, dc.gravity_of_earth, "Gravity of earth"),
        w(0x27, DType::F32, dc.scale_factor, "Scale factor"),
        w(0x28, DType::F32, dc.reference_load, "Reference load"),
        w(0x29, DType::U16, dc.steady_state_window, "Steady state window"),
        w(0x2A, DType::U32, dc.steady_state_tolerance, "Steady state tolerance"),
        w(0x31, DType::U16, dc.calibration_interval, "Calibration interval"),
    };
}

/// @brief Feste RxPDO-Zuordnung dieser Anwendung (0x1C12): RMB Control.
const std::vector<uint16_t>& rx_assignment() {
    static const std::vector<uint16_t> a = {kPdoControl};
    return a;
}

/// @brief Feste TxPDO-Zuordnung dieser Anwendung (0x1C13): RMB Status, RMB Value (Real).
const std::vector<uint16_t>& tx_assignment() {
    static const std::vector<uint16_t> a = {kPdoStatus, kPdoValueReal};
    return a;
}

/**
 * @brief Schreibliste für eine PDO-Zuordnung: Anzahl 0, Einträge, Anzahl.
 * @param sm_obj 0x1C12 oder 0x1C13
 * @param pdos zuzuordnende PDOs in Reihenfolge
 * @return SDO-Schreibzugriffe (nur in PRE-OP zulässig)
 */
std::vector<SdoWrite> assign_writes(uint16_t sm_obj, const std::vector<uint16_t>& pdos) {
    const char* what = sm_obj == kSm2Assign ? "RxPDO assign" : "TxPDO assign";
    std::vector<SdoWrite> out;
    out.push_back({sm_obj, 0, encode_value(DType::U8, 0), std::string(what) + ": Anzahl löschen",
                   DType::U8, true});
    for (size_t i = 0; i < pdos.size(); ++i)
        out.push_back({sm_obj, static_cast<uint8_t>(i + 1), encode_value(DType::U16, pdos[i]),
                       std::string(what) + ": " + known_pdo_name(pdos[i]), DType::U16, true});
    out.push_back({sm_obj, 0, encode_value(DType::U8, static_cast<double>(pdos.size())),
                   std::string(what) + ": Anzahl", DType::U8, true});
    return out;
}

/**
 * @brief Vollständige Sequenz des Übergangs PRE-OP -> SAFE-OP.
 * @param dc Klemmenparameter; 0x8000 nur bei write_on_start
 * @return PDO-Zuordnung 0x1C12/0x1C13, danach ggf. 0x8000
 */
std::vector<SdoWrite> init_sequence(const DeviceConfig& dc) {
    auto seq = assign_writes(kSm2Assign, rx_assignment());
    for (auto& w : assign_writes(kSm3Assign, tx_assignment())) seq.push_back(std::move(w));
    if (dc.write_on_start)
        for (auto& w : settings_writes(dc)) seq.push_back(std::move(w));
    return seq;
}

/// @brief UINT16 als Bytes, little endian.
std::vector<uint8_t> le_u16(uint16_t v) { return le<uint16_t>(v); }
/// @brief REAL32 als Bytes, little endian.
std::vector<uint8_t> le_f32(float v) { return le<float>(v); }
/// @brief REAL32 aus Bytes, little endian; 0 bei weniger als 4 Byte.
float f32_from_le(const std::vector<uint8_t>& d) {
    float f = 0.0f;
    if (d.size() >= sizeof(f)) std::memcpy(&f, d.data(), sizeof(f));
    return f;
}

} // namespace ew::el3356
