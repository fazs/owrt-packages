#include "el3356.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

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

/// Bekannter Objekteintrag laut ESI.
struct KnownEntry {
    uint16_t index;    ///< Objektindex
    uint8_t sub;       ///< Subindex
    const char* name;  ///< Name laut ESI
    DType type;        ///< Datentyp
};

/// Objektverzeichnis laut ESI (Revision 0x0019): RMB 0x6000/0x7000, AI-Kanäle 0x6010/0x6020
const KnownEntry kKnownEntries[] = {
    {kObjInputs, kSubInOverrange, "Overrange", DType::Bool},
    {kObjInputs, kSubInDataInvalid, "Data invalid", DType::Bool},
    {kObjInputs, kSubInError, "Error", DType::Bool},
    {kObjInputs, kSubInCalibration, "Calibration in progress", DType::Bool},
    {kObjInputs, kSubInSteadyState, "Steady state", DType::Bool},
    {kObjInputs, kSubInSyncError, "Sync error", DType::Bool},
    {kObjInputs, kSubInToggle, "TxPDO Toggle", DType::Bool},
    {kObjInputs, kSubInValue, "Value", DType::I32},
    {kObjInputs, kSubInValueReal, "Value (Real)", DType::F32},
    {kObjInputs, kSubInTimestamp, "Timestamp", DType::U64},
    {kObjAiInputsCh1, kSubAiUnderrange, "Underrange", DType::Bool},
    {kObjAiInputsCh1, kSubAiOverrange, "Overrange", DType::Bool},
    {kObjAiInputsCh1, kSubAiError, "Error", DType::Bool},
    {kObjAiInputsCh1, kSubAiToggle, "TxPDO Toggle", DType::Bool},
    {kObjAiInputsCh1, kSubAiValue, "Value", DType::I32},
    {kObjAiInputsCh2, kSubAiUnderrange, "Underrange", DType::Bool},
    {kObjAiInputsCh2, kSubAiOverrange, "Overrange", DType::Bool},
    {kObjAiInputsCh2, kSubAiError, "Error", DType::Bool},
    {kObjAiInputsCh2, kSubAiToggle, "TxPDO Toggle", DType::Bool},
    {kObjAiInputsCh2, kSubAiValue, "Value", DType::I32},
    {kObjSmOutParam, kSubSmOutSyncError, "Sync error", DType::Bool},
    {kObjTxPdoPar, kSubTxPdoParToggle, "TxPDO Toggle", DType::Bool},
    {kObjControl, kSubCtrlStartCalibration, "Start calibration", DType::Bool},
    {kObjControl, kSubCtrlDisableCalibration, "Disable calibration", DType::Bool},
    {kObjControl, kSubCtrlInputFreeze, "Input freeze", DType::Bool},
    {kObjControl, kSubCtrlSampleMode, "Sample mode", DType::Bool},
    {kObjControl, kSubCtrlTare, "Tara", DType::Bool},
    {kObjControl, kSubCtrlFilterFrequency, "Filter frequency", DType::U16},
};

/**
 * @brief PDO-Eintrag mit Name und Typ aus der ESI-Tabelle.
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

/**
 * @brief Lücke im Mapping.
 * @param bits Breite in Bit (laut ESI)
 * @return Eintrag ohne Objekt
 */
PdoEntry gap(uint8_t bits) {
    return known(kObjGap, kSubCount, bits);
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
    if (l.value_real.present && l.value_real.bit_len == kBitsDint)
        set_bits(img, l.value_real.bit_offset, kBitsDint, to_raw(DType::F32, value_real));
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
    case kPdoFilterFrequency: return "RMB Filter frequency";
    case kPdoStatus: return "RMB Status";
    case kPdoValueInt: return "RMB Value (INT32)";
    case kPdoValueReal: return "RMB Value (Real)";
    case kPdoTimestamp: return "RMB Timestamp";
    case kPdoAiStandardCh1: return "AI Standard Channel 1";
    case kPdoAiCompactCh1: return "AI Compact Channel 1";
    case kPdoAiStandardCh2: return "AI Standard Channel 2";
    case kPdoAiCompactCh2: return "AI Compact Channel 2";
    default: {
        char b[kPdoNameLen];
        std::snprintf(b, sizeof b, "0x%04X", pdo);
        return b;
    }
    }
}

/**
 * @brief PDOs der EL3356-0010 laut ESI (Revision 0x0019) mit Mapping und Exclude-Listen.
 *
 * Grundlage für die Simulation und Rückfall für Namen ohne SDO-Information.
 * Die Breiten der Lücken stammen aus dem Mapping der ESI.
 *
 * @return alle PDOs, nicht zugeordnet
 */
std::vector<PdoInfo> known_pdos() {
    auto pdo = [](uint16_t idx, bool rx, std::vector<PdoEntry> e, std::vector<uint16_t> ex) {
        return PdoInfo{idx, known_pdo_name(idx), rx, false, std::move(e), std::move(ex)};
    };
    auto ai_standard = [](uint16_t obj) {
        return std::vector<PdoEntry>{known(obj, kSubAiUnderrange, kBitsBool), known(obj, kSubAiOverrange, kBitsBool),
                                     gap(4U), known(obj, kSubAiError, kBitsBool), gap(1U), gap(7U),
                                     known(obj, kSubAiToggle, kBitsBool), known(obj, kSubAiValue, kBitsDint)};
    };
    const std::vector<uint16_t> ai = {kPdoAiStandardCh1, kPdoAiCompactCh1, kPdoAiStandardCh2, kPdoAiCompactCh2};
    const std::vector<uint16_t> rmb = {kPdoStatus, kPdoValueInt, kPdoValueReal, kPdoTimestamp};
    auto with = [](std::vector<uint16_t> a, uint16_t b) {
        a.insert(a.begin(), b);
        return a;
    };
    return {
        pdo(kPdoControl, true,
            {known(kObjControl, kSubCtrlStartCalibration, kBitsBool),
             known(kObjControl, kSubCtrlDisableCalibration, kBitsBool),
             known(kObjControl, kSubCtrlInputFreeze, kBitsBool), known(kObjControl, kSubCtrlSampleMode, kBitsBool),
             known(kObjControl, kSubCtrlTare, kBitsBool), gap(3U), gap(8U)},
            {}),
        pdo(kPdoFilterFrequency, true, {known(kObjControl, kSubCtrlFilterFrequency, kBitsUint)}, {}),
        pdo(kPdoStatus, false,
            {gap(1U), known(kObjInputs, kSubInOverrange, kBitsBool), gap(1U),
             known(kObjInputs, kSubInDataInvalid, kBitsBool), gap(2U), known(kObjInputs, kSubInError, kBitsBool),
             known(kObjInputs, kSubInCalibration, kBitsBool), known(kObjInputs, kSubInSteadyState, kBitsBool),
             gap(4U), known(kObjInputs, kSubInSyncError, kBitsBool), gap(1U),
             known(kObjInputs, kSubInToggle, kBitsBool)},
            ai),
        pdo(kPdoValueInt, false, {known(kObjInputs, kSubInValue, kBitsDint)}, with(ai, kPdoValueReal)),
        pdo(kPdoValueReal, false, {known(kObjInputs, kSubInValueReal, kBitsDint)}, with(ai, kPdoValueInt)),
        pdo(kPdoTimestamp, false, {known(kObjInputs, kSubInTimestamp, kBitsUlint)}, ai),
        pdo(kPdoAiStandardCh1, false, ai_standard(kObjAiInputsCh1), with(rmb, kPdoAiCompactCh1)),
        pdo(kPdoAiCompactCh1, false, {known(kObjAiInputsCh1, kSubAiValue, kBitsDint)}, with(rmb, kPdoAiStandardCh1)),
        pdo(kPdoAiStandardCh2, false, ai_standard(kObjAiInputsCh2), with(rmb, kPdoAiCompactCh2)),
        pdo(kPdoAiCompactCh2, false, {known(kObjAiInputsCh2, kSubAiValue, kBitsDint)}, with(rmb, kPdoAiStandardCh2)),
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
        w(kSubMode0EnableFilter, DType::Bool, dc.mode0_enable_filter, "Mode0 enable filter"),
        w(kSubMode0EnableAverager, DType::Bool, dc.mode0_enable_averager, "Mode0 enable averager"),
        w(kSubFilterMode0, DType::U16, dc.mode0_filter_settings, "Mode0 filter settings"),
        w(kSubRatedOutput, DType::F32, dc.rated_output_mv_v, "Rated output"),
        w(kSubNominalLoad, DType::F32, dc.nominal_load, "Nominal load"),
        w(kSubZeroBalance, DType::F32, dc.zero_balance, "Zero balance"),
        w(kSubGravity, DType::F32, dc.gravity_of_earth, "Gravity of earth"),
        w(kSubScaleFactor, DType::F32, dc.scale_factor, "Scale factor"),
        w(kSubReferenceLoad, DType::F32, dc.reference_load, "Reference load"),
        w(kSubSteadyStateWindow, DType::U16, dc.steady_state_window, "Steady state window"),
        w(kSubSteadyStateTolerance, DType::U32, dc.steady_state_tolerance, "Steady state tolerance"),
        w(kSubCalibrationInterval, DType::U16, dc.calibration_interval, "Calibration interval"),
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
    float f = 0.0F;
    if (d.size() >= sizeof(f)) std::memcpy(&f, d.data(), sizeof(f));
    return f;
}

} // namespace ew::el3356
