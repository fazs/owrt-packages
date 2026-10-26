#include "el3356.hpp"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <limits>
#include <map>
#include <string_view>

#include "device_description.hpp"

namespace ew::el3356 {

namespace {

constexpr uint32_t kBitsPerByte = 8U;                                    ///< Bits je Byte
constexpr uint8_t kMaxFieldBits = std::numeric_limits<uint64_t>::digits;  ///< längstes Bitfeld
constexpr uint32_t kMapIndexShift = 16U;   ///< Mapping-Eintrag: Index in Bit 16..31
constexpr uint32_t kMapSubShift = 8U;      ///< Mapping-Eintrag: Subindex in Bit 8..15
constexpr uint32_t kMapByteMask = 0xFFU;   ///< Maske für Subindex bzw. Bitlänge
constexpr size_t kObjNameLen = sizeof("0xIIII:SS");  ///< Puffer für "0xIIII:SS" inkl. Nullbyte
constexpr size_t kPdoNameLen = sizeof("0xIIII");     ///< Puffer für "0xIIII" inkl. Nullbyte

} // namespace

/**
 * @brief Zerlegt einen Mapping-Eintrag aus 0x16xx/0x1Axx.
 * @param raw UINT32: index<<16 | sub<<8 | bitlen
 * @return Index, Subindex, Bitlänge
 */
MapEntry decode_map_entry(uint32_t raw) {
    return {static_cast<uint16_t>(raw >> kMapIndexShift),
            static_cast<uint8_t>((raw >> kMapSubShift) & kMapByteMask),
            static_cast<uint8_t>(raw & kMapByteMask)};
}

/**
 * @brief Setzt einen Mapping-Eintrag für 0x16xx/0x1Axx zusammen (Umkehrung von decode_map_entry()).
 * @param e Index, Subindex, Bitlänge
 * @return UINT32: index<<16 | sub<<8 | bitlen
 */
uint32_t encode_map_entry(const MapEntry& e) {
    return static_cast<uint32_t>(e.index) << kMapIndexShift | static_cast<uint32_t>(e.sub) << kMapSubShift |
           static_cast<uint32_t>(e.bits);
}

/**
 * @brief Prüft, ob ein Index ein PDO-Mapping-Objekt ist (0x16xx oder 0x1Axx).
 * @param index Objektindex
 * @return true für RxPDO- und TxPDO-Mapping-Objekte
 */
bool is_pdo_mapping(uint16_t index) {
    const uint16_t group = index & kPdoGroupMask;
    return group == kPdoRxFirst || group == kPdoTxFirst;
}

/**
 * @brief Anzahl Bytes für ein Abbild von @p bits Bit.
 * @param bits Länge in Bit
 * @return aufgerundete Bytezahl
 */
size_t bytes_for_bits(uint32_t bits) {
    return (bits + kBitsPerByte - 1U) / kBitsPerByte;
}

namespace {

/**
 * @brief Durchläuft alle Einträge der PDOs in Zuordnungsreihenfolge.
 * @param pdos Mappings je PDO
 * @param on_entry Aufruf (Eintrag, Bitoffset ab Beginn des Abbilds)
 */
template <typename F>
void walk(const std::vector<std::vector<MapEntry>>& pdos, F&& on_entry) {
    uint32_t off = 0U;
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
    uint64_t v = 0U;
    for (uint8_t i = 0U; i < len && i < kMaxFieldBits; ++i, ++off)
        v |= static_cast<uint64_t>((img[off / kBitsPerByte] >> (off % kBitsPerByte)) & 1U) << i;
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
    for (uint8_t i = 0U; i < len && i < kMaxFieldBits; ++i, ++off) {
        const uint8_t mask = static_cast<uint8_t>(1U << (off % kBitsPerByte));
        if ((v >> i) & 1U)
            img[off / kBitsPerByte] |= mask;
        else
            img[off / kBitsPerByte] &= static_cast<uint8_t>(~mask);
    }
}

/// @brief Liest ein Bit; false, wenn der Eintrag nicht gemappt ist.
bool get_bit(const uint8_t* img, const EntryLoc& l) {
    if (!l.present) return false;
    return (img[l.bit_offset / kBitsPerByte] >> (l.bit_offset % kBitsPerByte)) & 1U;
}

/// @brief Setzt oder löscht ein Bit; ohne Wirkung, wenn der Eintrag nicht gemappt ist.
void set_bit(uint8_t* img, const EntryLoc& l, bool v) {
    if (!l.present) return;
    set_bits(img, l.bit_offset, kBitsBool, v ? 1U : 0U);
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
    case DType::Bool: return static_cast<double>(raw & 1U);
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
    case DType::Bool: return v != 0.0 ? 1U : 0U;
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

constexpr std::string_view kPdoNamePrefixSep = "__";  ///< ESI-Mappingnamen: "Status__Overrange"

/**
 * @brief Datentyp eines Eintrags der Gerätebeschreibung.
 * @param type Kurzname laut Beschreibung
 * @param bits Bitlänge, Rückfall für "string"/"bytes" und unbekannte Namen
 * @return Datentyp
 */
DType desc_type(const std::string& type, uint16_t bits) {
    DType t = DType::None;
    if (dtype_from_name(type, t)) return t;
    return dtype_from_coe(kCoeUnknown, static_cast<uint8_t>(std::min<uint16_t>(bits, kMaxFieldBits)));
}

/**
 * @brief PDO-Eintrag mit Name und Typ aus der Gerätebeschreibung.
 * @param index Objektindex (kObjGap = Lücke)
 * @param sub Subindex
 * @param bits Länge in Bit
 * @return Eintrag; unbekannte Objekte heißen "0xIIII:SS", Typ nach Bitlänge
 */
PdoEntry known(uint16_t index, uint8_t sub, uint8_t bits) {
    PdoEntry e;
    e.map = {index, sub, bits};
    if (!known_entry(index, sub, e.name, e.type) && index != kObjGap) {
        e.type = dtype_from_coe(kCoeUnknown, bits);
        char b[kObjNameLen];
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
 * @param coe ECT_*-Code aus der SDO-Information, kCoeUnknown = unbekannt
 * @param bits Bitlänge, Rückfall bei unbekanntem Code
 * @return Datentyp
 */
DType dtype_from_coe(uint16_t coe, uint8_t bits) {
    switch (coe) {
    case kCoeBoolean: return DType::Bool;
    case kCoeInteger8: return DType::I8;
    case kCoeInteger16: return DType::I16;
    case kCoeInteger32: return DType::I32;
    case kCoeUnsigned8: return DType::U8;
    case kCoeUnsigned16: return DType::U16;
    case kCoeUnsigned32: return DType::U32;
    case kCoeReal32: return DType::F32;
    case kCoeUnsigned64: return DType::U64;
    default: break;
    }
    if (bits == kBitsBool) return DType::Bool;
    if (bits <= std::numeric_limits<uint8_t>::digits) return DType::U8;
    if (bits <= std::numeric_limits<uint16_t>::digits) return DType::U16;
    if (bits <= std::numeric_limits<uint32_t>::digits) return DType::U32;
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
    case DType::U8: return sizeof(uint8_t);
    case DType::I8: return sizeof(int8_t);
    case DType::U16: return sizeof(uint16_t);
    case DType::I16: return sizeof(int16_t);
    case DType::U32: return sizeof(uint32_t);
    case DType::I32: return sizeof(int32_t);
    case DType::F32: return sizeof(float);
    case DType::U64: return sizeof(uint64_t);
    default: return 0U;
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
    for (size_t i = 0U; i < out.size(); ++i) out[i] = static_cast<uint8_t>(raw >> (kBitsPerByte * i));
    return out;
}

/**
 * @brief Dekodiert SDO-Daten.
 * @param t Datentyp
 * @param d Daten, little endian; überzählige Bytes werden ignoriert
 * @return Wert
 */
double decode_value(DType t, const std::vector<uint8_t>& d) {
    uint64_t raw = 0U;
    const size_t n = dtype_size(t) != 0U ? dtype_size(t) : sizeof(uint64_t);
    for (size_t i = 0U; i < d.size() && i < n; ++i)
        raw |= static_cast<uint64_t>(d[i]) << (kBitsPerByte * i);
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
        if (e.index == kObjSmOutParam && e.sub == kSubSmOutSyncError) l.sync_error = loc;
        if (e.index == kObjTxPdoPar && e.sub == kSubTxPdoParToggle) l.toggle = loc;
        if (e.index != kObjInputs) return;
        switch (e.sub) {
        case kSubInOverrange: l.overrange = loc; break;
        case kSubInDataInvalid: l.data_invalid = loc; break;
        case kSubInError: l.error = loc; break;
        case kSubInCalibration: l.calibration = loc; break;
        case kSubInSteadyState: l.steady_state = loc; break;
        case kSubInSyncError: l.sync_error = loc; break;
        case kSubInToggle: l.toggle = loc; break;
        case kSubInValue: l.value_int = loc; break;
        case kSubInValueReal: l.value_real = loc; break;
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
        case kSubCtrlStartCalibration: l.start_calibration = loc; break;
        case kSubCtrlDisableCalibration: l.disable_calibration = loc; break;
        case kSubCtrlInputFreeze: l.input_freeze = loc; break;
        case kSubCtrlSampleMode: l.sample_mode = loc; break;
        case kSubCtrlTare: l.tare = loc; break;
        case kSubCtrlFilterFrequency: l.filter_frequency = loc; break;
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
    float f = 0.0F;
    if (!l.value_real.present || l.value_real.bit_len != kBitsDint ||
        (l.value_real.bit_offset % kBitsPerByte) != 0U)
        return f;
    std::memcpy(&f, img + l.value_real.bit_offset / kBitsPerByte, sizeof(f));
    return f;
}

/**
 * @brief Messwert der Klemme: Value (Real) 0x6000:12, sonst Value (INT32) 0x6000:11.
 * @param img Eingangsabbild des Slaves
 * @param l Lage der Einträge
 * @return Wert, 0 wenn keiner byteweise mit 32 Bit gemappt ist
 */
double decode_measurement(const uint8_t* img, const InputLayout& l) {
    if (l.value_real.present) return decode_value_real(img, l);
    if (!l.value_int.present || l.value_int.bit_len != kBitsDint || (l.value_int.bit_offset % kBitsPerByte) != 0U)
        return 0.0;
    return from_raw(DType::I32, get_bits(img, l.value_int.bit_offset, kBitsDint));
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
    if (l.filter_frequency.present)
        set_bits(img, l.filter_frequency.bit_offset, l.filter_frequency.bit_len, c.filter_frequency);
}

/**
 * @brief Schreibt Statusbits und Messwert in ein Eingangsabbild (Simulation).
 * @param img Eingangsabbild
 * @param l Lage der Einträge
 * @param s Statusbits
 * @param value_real Messwert; Value (INT32) gerundet
 */
void encode_inputs(uint8_t* img, const InputLayout& l, const El3356Status& s, float value_real) {
    set_bit(img, l.overrange, s.overrange);
    set_bit(img, l.data_invalid, s.data_invalid);
    set_bit(img, l.error, s.error);
    set_bit(img, l.calibration, s.calibration_in_progress);
    set_bit(img, l.steady_state, s.steady_state);
    set_bit(img, l.sync_error, s.sync_error);
    set_bit(img, l.toggle, s.txpdo_toggle);
    if (l.value_real.present && l.value_real.bit_len == kBitsDint)
        set_bits(img, l.value_real.bit_offset, kBitsDint, to_raw(DType::F32, value_real));
    if (l.value_int.present && l.value_int.bit_len == kBitsDint)
        set_bits(img, l.value_int.bit_offset, kBitsDint, to_raw(DType::I32, value_real));
}

/**
 * @brief Zugriff auf das Steuerbit 0x7000:@p sub.
 * @param c Steuerbits
 * @param sub Subindex kSubCtrlStartCalibration..kSubCtrlTare
 * @return Zeiger auf das Feld in @p c, nullptr bei unbekanntem Subindex
 */
bool* control_bit(El3356Control& c, uint8_t sub) {
    switch (sub) {
    case kSubCtrlStartCalibration: return &c.start_calibration;
    case kSubCtrlDisableCalibration: return &c.disable_calibration;
    case kSubCtrlInputFreeze: return &c.input_freeze;
    case kSubCtrlSampleMode: return &c.sample_mode;
    case kSubCtrlTare: return &c.tare;
    default: return nullptr;
    }
}

/**
 * @brief Name und Typ eines Eintrags laut Gerätebeschreibung.
 *
 * Gesucht wird im Objektverzeichnis, danach in den PDO-Mappings aller Revisionen (z. B.
 * 0x1800:09 bis Revision 0x0014, das kein Objekteintrag ist); dort ohne Präfix "Status__".
 *
 * @param index Objektindex
 * @param sub Subindex
 * @param[out] name Name
 * @param[out] type Datentyp
 * @return false, wenn der Eintrag unbekannt ist
 */
bool known_entry(uint16_t index, uint8_t sub, std::string& name, DType& type) {
    const auto& d = DeviceDescription::el3356_0010();
    if (const auto* e = d.entry(index, sub)) {
        name = e->name;
        type = desc_type(e->type, e->bits);
        return true;
    }
    if (index == kObjGap) return false;
    for (const auto& p : d.pdos())
        for (const auto& l : p.layouts)
            for (const auto& e : l.entries)
                if (e.index == index && e.sub == sub) {
                    const auto sep = e.name.find(kPdoNamePrefixSep);
                    name = sep == std::string::npos ? e.name : e.name.substr(sep + kPdoNamePrefixSep.size());
                    type = desc_type(e.type, e.bits);
                    return true;
                }
    return false;
}

/**
 * @brief Name eines PDOs laut Gerätebeschreibung.
 * @param pdo Index 0x16xx/0x1Axx
 * @return Name, sonst "0xIIII"
 */
std::string known_pdo_name(uint16_t pdo) {
    if (const auto* p = DeviceDescription::el3356_0010().pdo(pdo)) return p->name;
    char b[kPdoNameLen];
    std::snprintf(b, sizeof b, "0x%04X", pdo);
    return b;
}

/**
 * @brief PDOs der EL3356-0010 laut Gerätebeschreibung mit Mapping und Exclude-Listen.
 *
 * Mapping der neuesten Revision. Grundlage für die Simulation und Rückfall für Namen ohne
 * SDO-Information.
 *
 * @return alle PDOs der neuesten Revision, nicht zugeordnet
 */
std::vector<PdoInfo> known_pdos() {
    const auto& d = DeviceDescription::el3356_0010();
    std::vector<PdoInfo> out;
    for (const auto& p : d.pdos()) {
        const auto* lay = d.layout(p);
        if (lay == nullptr) continue;
        PdoInfo info{p.index, p.name, p.rx, false, {}, p.exclude};
        for (const auto& e : lay->entries) info.entries.push_back(known(e.index, e.sub, e.bits));
        out.push_back(std::move(info));
    }
    return out;
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
        uint32_t off = 0U;
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

namespace {

/// @name Konfigurationsbereich des Objektverzeichnisses (ETG.5001 Modular Device Profile)
/// @{
constexpr uint16_t kObjConfigFirst = 0x8000U;  ///< erstes Konfigurationsobjekt
constexpr uint16_t kObjConfigLast = 0x8FFFU;   ///< letztes Konfigurationsobjekt
/// @}
constexpr size_t kNumTextLen = 32U;  ///< Puffer für eine formatierte Zahl
constexpr int kHexBase = 16;         ///< Basis der Adressen "0xIIII" der Gerätebeschreibung

/**
 * @brief Adresse als "0xIIII:SS" (Schlüssel in DeviceConfig::coe und der Gerätebeschreibung).
 * @param index Objektindex
 * @param sub Subindex
 * @return Adresse
 */
std::string obj_key(uint16_t index, uint8_t sub) {
    char b[kObjNameLen];
    std::snprintf(b, sizeof b, "0x%04X:%02X", index, sub);
    return b;
}

/**
 * @brief Zahl für Meldungen (Dezimalkomma, ohne überflüssige Stellen).
 * @param v Wert
 * @return Text
 */
std::string num_text(double v) {
    char b[kNumTextLen];
    std::snprintf(b, sizeof b, "%.10g", v);
    std::string t = b;
    std::replace(t.begin(), t.end(), '.', ',');
    return t;
}

/**
 * @brief JSON-Wert (Zahl oder bool) als Zahl.
 * @param j Wert
 * @return Zahl; bool als 0/1
 */
double json_number(const nlohmann::json& j) {
    return j.is_boolean() ? (j.get<bool>() ? 1.0 : 0.0) : j.get<double>();
}

/**
 * @brief Prüft, ob ein Eintrag ein schreibbarer Klemmenparameter ist.
 * @param e Eintrag der Gerätebeschreibung
 * @return true für rw-Einträge 0x8000..0x8FFF mit Zahlentyp, ohne Subindex 0 und ohne never_write
 */
bool is_setting(const DescEntry& e) {
    if (e.index < kObjConfigFirst || e.index > kObjConfigLast || e.sub == kSubCount) return false;
    if (e.raw.value("access", "") != "rw" || e.raw.contains("never_write")) return false;
    DType t = DType::None;
    return dtype_from_name(e.type, t);
}

/**
 * @brief Prüft einen Wert gegen Datentyp, Grenzen und Aufzählung laut Gerätebeschreibung.
 * @param e Eintrag
 * @param t Datentyp
 * @param v Wert (CoE-Rohwert)
 * @return Fehlertext, leer = gültig
 */
std::string check_value(const DescEntry& e, DType t, double v) {
    auto range = [](double lo, double hi) { return "zulässig " + num_text(lo) + ".." + num_text(hi); };
    auto int_range = [&](auto lim) -> std::string {
        using L = decltype(lim);
        if (std::floor(v) != v) return "Ganzzahl erwartet";
        if (v < static_cast<double>(std::numeric_limits<L>::min()) ||
            v > static_cast<double>(std::numeric_limits<L>::max()))
            return range(static_cast<double>(std::numeric_limits<L>::min()),
                         static_cast<double>(std::numeric_limits<L>::max()));
        return {};
    };
    std::string err;
    switch (t) {
    case DType::Bool: err = (v == 0.0 || v == 1.0) ? "" : "0 oder 1 erwartet"; break;
    case DType::U8: err = int_range(uint8_t{}); break;
    case DType::U16: err = int_range(uint16_t{}); break;
    case DType::U32: err = int_range(uint32_t{}); break;
    case DType::I8: err = int_range(int8_t{}); break;
    case DType::I16: err = int_range(int16_t{}); break;
    case DType::I32: err = int_range(int32_t{}); break;
    case DType::F32: err = std::isfinite(v) && std::fabs(v) <= FLT_MAX ? "" : "kein gültiger REAL32-Wert"; break;
    default: err = "Datentyp nicht unterstützt"; break;
    }
    if (!err.empty()) return err;
    const double lo = e.raw.contains("min") ? json_number(e.raw.at("min")) : -HUGE_VAL;
    const double hi = e.raw.contains("max") ? json_number(e.raw.at("max")) : HUGE_VAL;
    if (v < lo || v > hi) return range(lo, hi);
    if (e.raw.contains("enum")) {
        const auto& items = DeviceDescription::el3356_0010().json().at("enums").at(e.raw.at("enum").get<std::string>());
        const bool hit = std::any_of(items.begin(), items.end(),
                                     [&](const nlohmann::json& it) { return json_number(it.at("value")) == v; });
        if (!hit) {
            std::string vals;
            for (const auto& it : items) vals += (vals.empty() ? "" : ", ") + num_text(json_number(it.at("value")));
            return "zulässig " + vals;
        }
    }
    return {};
}

} // namespace

/**
 * @brief Schreibbare Klemmenparameter laut Gerätebeschreibung mit dem wirksamen Wert.
 *
 * Wert: typisiertes Feld von DeviceConfig (config_key), sonst DeviceConfig::coe, sonst Default
 * der ESI. Einträge ohne Wert und Einträge, die in der Revision fehlen, entfallen.
 *
 * @param dc Klemmenparameter
 * @param revision Revision der Klemme, 0 = neueste der Beschreibung
 * @return Parameter nach Index und Subindex
 */
std::vector<DeviceSetting> device_settings(const DeviceConfig& dc, uint32_t revision) {
    const auto& d = DeviceDescription::el3356_0010();
    const nlohmann::json typed = device_json(dc);
    std::vector<DeviceSetting> out;
    for (const auto& e : d.entries()) {
        if (!is_setting(e) || !d.in_revision(e.revisions, revision)) continue;
        DeviceSetting s{e.index, e.sub, e.name, "", desc_type(e.type, e.bits), 0.0, false};
        const std::string key = obj_key(e.index, e.sub);
        if (e.raw.contains("config_key")) {
            const auto ck = e.raw.at("config_key").get<std::string>();
            s.key = "device." + ck;
            s.value = json_number(typed.at(ck));
            s.configured = true;
        } else if (const auto it = dc.coe.find(key); it != dc.coe.end()) {
            s.key = "device.coe." + key;
            s.value = it->second;
            s.configured = true;
        } else if (e.raw.contains("default")) {
            s.key = "device.coe." + key;
            s.value = json_number(e.raw.at("default"));
        } else {
            continue;
        }
        out.push_back(std::move(s));
    }
    return out;
}

/**
 * @brief Prüft die Klemmenparameter gegen die Gerätebeschreibung.
 *
 * Schlüssel in DeviceConfig::coe müssen schreibbare Klemmenparameter ohne typisiertes Feld
 * sein; alle konfigurierten Werte werden gegen Datentyp, Grenzen und Aufzählung geprüft.
 *
 * @param dc Klemmenparameter
 * @return Fehlermeldungen; leer = gültig
 */
std::vector<std::string> validate_device(const DeviceConfig& dc) {
    const auto& d = DeviceDescription::el3356_0010();
    std::vector<std::string> errs;
    for (const auto& kv : dc.coe) {
        const std::string field = "device.coe." + kv.first;
        unsigned idx = 0U, sub = 0U;
        const bool parsed = std::sscanf(kv.first.c_str(), "0x%4X:%2X", &idx, &sub) == 2 &&
                            kv.first == obj_key(static_cast<uint16_t>(idx), static_cast<uint8_t>(sub));
        const DescEntry* e = parsed ? d.entry(static_cast<uint16_t>(idx), static_cast<uint8_t>(sub)) : nullptr;
        if (!parsed)
            errs.push_back(field + ": Schlüssel im Format 0xIIII:SS erwartet");
        else if (e == nullptr)
            errs.push_back(field + ": Eintrag nicht in der Gerätebeschreibung");
        else if (e->raw.contains("never_write"))
            errs.push_back(field + " (" + e->name + "): darf nicht geschrieben werden (" +
                           e->raw.at("never_write").get<std::string>() + ")");
        else if (!is_setting(*e))
            errs.push_back(field + " (" + e->name + "): kein schreibbarer Klemmenparameter");
        else if (e->raw.contains("config_key"))
            errs.push_back(field + " (" + e->name + "): über device." +
                           e->raw.at("config_key").get<std::string>() + " einstellen");
    }
    if (variant_name(dc.pdo_variant) == nullptr)
        errs.push_back("device.pdo_variant: '" + std::string(kVariantReal) + "' oder '" + kVariantInt32 + "'");
    else
        for (auto& m : check_assignment(pdo_assignment(dc))) errs.push_back("device.pdo_variant: " + m);
    const auto a = pdo_assignment(dc);
    if (std::find(a.rx.begin(), a.rx.end(), kPdoFilterFrequency) != a.rx.end())
        if (const auto* ff = d.entry(kObjControl, kSubCtrlFilterFrequency)) {
            const auto err = check_value(*ff, DType::U16, dc.filter_frequency);
            if (!err.empty()) errs.push_back("device.filter_frequency (0x7000:11 " + ff->name + "): " + err);
        }
    for (const auto& s : device_settings(dc)) {
        if (!s.configured) continue;
        const auto err = check_value(*d.entry(s.index, s.sub), s.type, s.value);
        if (!err.empty()) errs.push_back(s.key + " (" + obj_key(s.index, s.sub) + " " + s.name + "): " + err);
    }
    return errs;
}

/**
 * @brief Schreibliste aller Klemmenparameter (wie eine Startup-Liste in TwinCAT).
 * @param dc Klemmenparameter
 * @param revision Revision der Klemme, 0 = neueste der Beschreibung
 * @return SDO-Schreibzugriffe nach Index und Subindex (auch in OP zulässig)
 */
std::vector<SdoWrite> settings_writes(const DeviceConfig& dc, uint32_t revision) {
    std::vector<SdoWrite> out;
    for (const auto& s : device_settings(dc, revision))
        out.push_back({s.index, s.sub, encode_value(s.type, s.value), s.name, s.type, false});
    return out;
}

/**
 * @brief ESI-Name der Prozessdaten-Variante.
 * @param variant DeviceConfig::pdo_variant
 * @return Name der AlternativeSmMapping, nullptr bei unbekannter Variante
 */
const char* variant_name(const std::string& variant) {
    if (variant == kVariantReal) return kVariantRealName;
    if (variant == kVariantInt32) return kVariantInt32Name;
    return nullptr;
}

/**
 * @brief PDO-Zuordnung laut Gerätebeschreibung.
 *
 * Grundlage ist die Variante (AlternativeSmMapping); PDOs aus pdo_rules (0x1601 bei Filter
 * „PDO Filter frequency“) kommen hinzu, wenn eine Bedingung erfüllt ist. PDOs, die in der
 * Revision fehlen, entfallen.
 *
 * @param dc Klemmenparameter (Variante, Filtereinstellungen)
 * @param revision Revision der Klemme, 0 = neueste der Beschreibung
 * @return Zuordnung; leer bei unbekannter Variante
 */
PdoAssignment pdo_assignment(const DeviceConfig& dc, uint32_t revision) {
    const auto& d = DeviceDescription::el3356_0010();
    PdoAssignment a;
    const char* name = variant_name(dc.pdo_variant);
    if (name == nullptr) return a;
    auto add = [&](std::vector<uint16_t>& list, const nlohmann::json& idx) {
        const auto pdo = static_cast<uint16_t>(std::stoul(idx.get<std::string>(), nullptr, kHexBase));
        const auto* p = d.pdo(pdo);
        if (p != nullptr && d.layout(*p, revision) != nullptr &&
            std::find(list.begin(), list.end(), pdo) == list.end())
            list.push_back(pdo);
    };
    for (const auto& v : d.json().at("variants")) {
        if (v.at("name") != name) continue;
        for (const auto& i : v.value("sm2", nlohmann::json::array())) add(a.rx, i);
        for (const auto& i : v.value("sm3", nlohmann::json::array())) add(a.tx, i);
    }
    std::map<std::string, double> values;
    for (const auto& s : device_settings(dc, revision)) values[obj_key(s.index, s.sub)] = s.value;
    for (const auto& rule : d.json().at("pdo_rules")) {
        const bool on = std::any_of(rule.at("if_any").begin(), rule.at("if_any").end(), [&](const nlohmann::json& c) {
            const auto it = values.find(c.at("obj").get<std::string>());
            return it != values.end() && it->second == json_number(c.at("eq"));
        });
        if (!on) continue;
        const auto* p = d.pdo(static_cast<uint16_t>(std::stoul(rule.at("pdo").get<std::string>(), nullptr, kHexBase)));
        if (p != nullptr) add(p->rx ? a.rx : a.tx, rule.at("pdo"));
    }
    return a;
}

/**
 * @brief Prüft eine Zuordnung gegen die Exclude-Listen und die Revision der Gerätebeschreibung.
 * @param a Zuordnung
 * @param revision Revision der Klemme, 0 = neueste der Beschreibung
 * @return Fehlermeldungen; leer = gültig
 */
std::vector<std::string> check_assignment(const PdoAssignment& a, uint32_t revision) {
    const auto& d = DeviceDescription::el3356_0010();
    std::vector<std::string> errs;
    std::vector<uint16_t> all = a.rx;
    all.insert(all.end(), a.tx.begin(), a.tx.end());
    if (a.tx.empty()) errs.emplace_back("keine TxPDOs zugeordnet");
    for (uint16_t idx : all) {
        const auto* p = d.pdo(idx);
        if (p == nullptr || d.layout(*p, revision) == nullptr) {
            errs.push_back(known_pdo_name(idx) + ": in der Revision nicht vorhanden");
            continue;
        }
        for (uint16_t other : all)
            if (other > idx && std::find(p->exclude.begin(), p->exclude.end(), other) != p->exclude.end())
                errs.push_back(p->name + " und " + known_pdo_name(other) + " schließen sich aus (ESI Exclude)");
    }
    return errs;
}

/**
 * @brief Schreibliste für eine PDO-Zuordnung: Anzahl 0, Einträge, Anzahl.
 * @param sm_obj 0x1C12 oder 0x1C13
 * @param pdos zuzuordnende PDOs in Reihenfolge
 * @return pdos.size() + 2 SDO-Schreibzugriffe (nur in PRE-OP zulässig)
 */
std::vector<SdoWrite> assign_writes(uint16_t sm_obj, const std::vector<uint16_t>& pdos) {
    const char* what = sm_obj == kSm2Assign ? "RxPDO assign" : "TxPDO assign";
    std::vector<SdoWrite> out;
    out.push_back({sm_obj, kSubCount, encode_value(DType::U8, 0.0), std::string(what) + ": Anzahl löschen",
                   DType::U8, true});
    for (size_t i = 0U; i < pdos.size(); ++i)
        out.push_back({sm_obj, static_cast<uint8_t>(kSubFirstEntry + i), encode_value(DType::U16, pdos[i]),
                       std::string(what) + ": " + known_pdo_name(pdos[i]), DType::U16, true});
    out.push_back({sm_obj, kSubCount, encode_value(DType::U8, static_cast<double>(pdos.size())),
                   std::string(what) + ": Anzahl", DType::U8, true});
    return out;
}

/**
 * @brief Vollständige Sequenz des Übergangs PRE-OP -> SAFE-OP.
 * @param dc Klemmenparameter (Variante, Filter); 0x8000.. nur bei write_on_start
 * @param revision Revision der Klemme, 0 = neueste der Beschreibung
 * @return PDO-Zuordnung 0x1C12/0x1C13, danach ggf. 0x8000
 */
std::vector<SdoWrite> init_sequence(const DeviceConfig& dc, uint32_t revision) {
    const auto a = pdo_assignment(dc, revision);
    auto seq = assign_writes(kSm2Assign, a.rx);
    for (auto& w : assign_writes(kSm3Assign, a.tx)) seq.push_back(std::move(w));
    if (dc.write_on_start)
        for (auto& w : settings_writes(dc, revision)) seq.push_back(std::move(w));
    return seq;
}

/// @brief UINT16 als Bytes, little endian.
std::vector<uint8_t> le_u16(uint16_t v) { return le<uint16_t>(v); }
/// @brief REAL32 als Bytes, little endian.
std::vector<uint8_t> le_f32(float v) { return le<float>(v); }
/// @brief REAL32 aus Bytes, little endian; 0 bei weniger als 4 Byte.
float f32_from_le(const std::vector<uint8_t>& d) {
    float f = 0.0F;
    if (d.size() >= sizeof(f)) std::memcpy(&f, d.data(), sizeof(f));
    return f;
}

} // namespace ew::el3356
