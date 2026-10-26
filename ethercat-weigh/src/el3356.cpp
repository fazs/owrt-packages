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

constexpr uint32_t kBitsPerByte = 8U;                                    ///< bits per byte
constexpr uint8_t kMaxFieldBits = std::numeric_limits<uint64_t>::digits;  ///< longest bit field
constexpr uint32_t kMapIndexShift = 16U;   ///< mapping entry: index in bits 16..31
constexpr uint32_t kMapSubShift = 8U;      ///< mapping entry: subindex in bits 8..15
constexpr uint32_t kMapByteMask = 0xFFU;   ///< mask for subindex or bit length
constexpr size_t kObjNameLen = sizeof("0xIIII:SS");  ///< buffer for "0xIIII:SS" incl. null byte
constexpr size_t kPdoNameLen = sizeof("0xIIII");     ///< buffer for "0xIIII" incl. null byte

} // namespace

/**
 * @brief Splits a mapping entry from 0x16xx/0x1Axx.
 * @param raw UINT32: index<<16 | sub<<8 | bitlen
 * @return index, subindex, bit length
 */
MapEntry decode_map_entry(uint32_t raw) {
    return {static_cast<uint16_t>(raw >> kMapIndexShift),
            static_cast<uint8_t>((raw >> kMapSubShift) & kMapByteMask),
            static_cast<uint8_t>(raw & kMapByteMask)};
}

/**
 * @brief Composes a mapping entry for 0x16xx/0x1Axx (inverse of decode_map_entry()).
 * @param e index, subindex, bit length
 * @return UINT32: index<<16 | sub<<8 | bitlen
 */
uint32_t encode_map_entry(const MapEntry& e) {
    return static_cast<uint32_t>(e.index) << kMapIndexShift | static_cast<uint32_t>(e.sub) << kMapSubShift |
           static_cast<uint32_t>(e.bits);
}

/**
 * @brief Checks whether an index is a PDO mapping object (0x16xx or 0x1Axx).
 * @param index object index
 * @return true for RxPDO and TxPDO mapping objects
 */
bool is_pdo_mapping(uint16_t index) {
    const uint16_t group = index & kPdoGroupMask;
    return group == kPdoRxFirst || group == kPdoTxFirst;
}

/**
 * @brief Number of bytes for an image of @p bits bits.
 * @param bits length in bits
 * @return rounded-up byte count
 */
size_t bytes_for_bits(uint32_t bits) {
    return (bits + kBitsPerByte - 1U) / kBitsPerByte;
}

namespace {

/**
 * @brief Iterates over all entries of the PDOs in assignment order.
 * @param pdos mappings per PDO
 * @param on_entry callback (entry, bit offset from the start of the image)
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
 * @brief Reads a bit field (little endian, up to 64 bits).
 * @param img process image
 * @param off bit offset
 * @param len length in bits
 * @return raw value
 */
uint64_t get_bits(const uint8_t* img, uint32_t off, uint8_t len) {
    uint64_t v = 0U;
    for (uint8_t i = 0U; i < len && i < kMaxFieldBits; ++i, ++off)
        v |= static_cast<uint64_t>((img[off / kBitsPerByte] >> (off % kBitsPerByte)) & 1U) << i;
    return v;
}

/**
 * @brief Writes a bit field (little endian, up to 64 bits).
 * @param img process image
 * @param off bit offset
 * @param len length in bits
 * @param v raw value
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

/// @brief Reads a bit; false if the entry is not mapped.
bool get_bit(const uint8_t* img, const EntryLoc& l) {
    if (!l.present) return false;
    return (img[l.bit_offset / kBitsPerByte] >> (l.bit_offset % kBitsPerByte)) & 1U;
}

/// @brief Sets or clears a bit; no effect if the entry is not mapped.
void set_bit(uint8_t* img, const EntryLoc& l, bool v) {
    if (!l.present) return;
    set_bits(img, l.bit_offset, kBitsBool, v ? 1U : 0U);
}

/// @brief Bytes of a value in memory order (target platforms are little endian).
template <typename T>
std::vector<uint8_t> le(T v) {
    std::vector<uint8_t> out(sizeof(T));
    std::memcpy(out.data(), &v, sizeof(T));  // target platforms are little endian
    return out;
}

/**
 * @brief Raw value (up to 64 bits, little endian) as a number according to the type.
 * @param t data type
 * @param raw raw bits
 * @return value; U64 rounded as double
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
 * @brief Number as raw value according to the type (integers rounded, unsigned from 0).
 * @param t data type
 * @param v value
 * @return raw bits
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

constexpr std::string_view kPdoNamePrefixSep = "__";  ///< ESI mapping names: "Status__Overrange"

/**
 * @brief Data type of an entry of the device description.
 * @param type short name according to the description
 * @param bits bit length, fallback for "string"/"bytes" and unknown names
 * @return data type
 */
DType desc_type(const std::string& type, uint16_t bits) {
    DType t = DType::None;
    if (dtype_from_name(type, t)) return t;
    return dtype_from_coe(kCoeUnknown, static_cast<uint8_t>(std::min<uint16_t>(bits, kMaxFieldBits)));
}

/**
 * @brief PDO entry with name and type from the device description.
 * @param index object index (kObjGap = gap)
 * @param sub subindex
 * @param bits length in bits
 * @return entry; unknown objects are named "0xIIII:SS", type by bit length
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
 * @brief Short name of a data type for JSON.
 * @param t data type
 * @return "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "f32" or "" for None
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
 * @brief Data type from its short name.
 * @param s short name (see dtype_name())
 * @param[out] t data type
 * @return false for an unknown name
 */
bool dtype_from_name(const std::string& s, DType& t) {
    for (DType c : kValueTypes)
        if (s == dtype_name(c)) {
            t = c;
            return true;
        }
    return false;
}

/**
 * @brief Data type from the CoE data type code (ETG.1000.6).
 * @param coe ECT_* code from the SDO information, kCoeUnknown = unknown
 * @param bits bit length, fallback for an unknown code
 * @return data type
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
 * @brief Size for SDO accesses.
 * @param t data type
 * @return bytes, 0 for None
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
 * @brief Encodes a value for an SDO write access.
 * @param t data type
 * @param v value
 * @return dtype_size(t) bytes, little endian
 */
std::vector<uint8_t> encode_value(DType t, double v) {
    const uint64_t raw = to_raw(t, v);
    std::vector<uint8_t> out(dtype_size(t));
    for (size_t i = 0U; i < out.size(); ++i) out[i] = static_cast<uint8_t>(raw >> (kBitsPerByte * i));
    return out;
}

/**
 * @brief Decodes SDO data.
 * @param t data type
 * @param d data, little endian; surplus bytes are ignored
 * @return value
 */
double decode_value(DType t, const std::vector<uint8_t>& d) {
    uint64_t raw = 0U;
    const size_t n = dtype_size(t) != 0U ? dtype_size(t) : sizeof(uint64_t);
    for (size_t i = 0U; i < d.size() && i < n; ++i)
        raw |= static_cast<uint64_t>(d[i]) << (kBitsPerByte * i);
    return from_raw(t, raw);
}

/**
 * @brief Computes the position of the evaluated inputs.
 *
 * Detects sync error and TxPDO toggle both in 0x6000 and (up to revision 0x0014)
 * in 0x1C32:20 or 0x1800:09.
 *
 * @param pdos mappings of the TxPDOs in the order of 0x1C13
 * @return position relative to the start of the input image
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
 * @brief Computes the position of the control bits 0x7000.
 * @param pdos mappings of the RxPDOs in the order of 0x1C12
 * @return position relative to the start of the output image
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
 * @brief Reads the status bits from the input image.
 * @param img input image of the slave
 * @param l position of the entries
 * @return status bits; unmapped bits are false
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
 * @brief Reads Value (Real) 0x6000:12 from the input image.
 * @param img input image of the slave
 * @param l position of the entries
 * @return value, 0 if not mapped byte-aligned with 32 bits
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
 * @brief Measured value of the terminal: Value (Real) 0x6000:12, otherwise Value (INT32) 0x6000:11.
 * @param img input image of the slave
 * @param l position of the entries
 * @return value, 0 if neither is mapped byte-aligned with 32 bits
 */
double decode_measurement(const uint8_t* img, const InputLayout& l) {
    if (l.value_real.present) return decode_value_real(img, l);
    if (!l.value_int.present || l.value_int.bit_len != kBitsDint || (l.value_int.bit_offset % kBitsPerByte) != 0U)
        return 0.0;
    return from_raw(DType::I32, get_bits(img, l.value_int.bit_offset, kBitsDint));
}

/**
 * @brief Writes the control bits into the output image.
 * @param img output image of the slave
 * @param l position of the control bits
 * @param c control bits
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
 * @brief Writes status bits and measured value into an input image (simulation).
 * @param img input image
 * @param l position of the entries
 * @param s status bits
 * @param value_real measured value; Value (INT32) rounded
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
 * @brief Access to the control bit 0x7000:@p sub.
 * @param c control bits
 * @param sub subindex kSubCtrlStartCalibration..kSubCtrlTare
 * @return pointer to the field in @p c, nullptr for an unknown subindex
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
 * @brief Name and type of an entry according to the device description.
 *
 * The object dictionary is searched first, then the PDO mappings of all revisions (e.g.
 * 0x1800:09 up to revision 0x0014, which is not an object entry); there without the prefix "Status__".
 *
 * @param index object index
 * @param sub subindex
 * @param[out] name name
 * @param[out] type data type
 * @return false if the entry is unknown
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
 * @brief Name of a PDO according to the device description.
 * @param pdo index 0x16xx/0x1Axx
 * @return name, otherwise "0xIIII"
 */
std::string known_pdo_name(uint16_t pdo) {
    if (const auto* p = DeviceDescription::el3356_0010().pdo(pdo)) return p->name;
    char b[kPdoNameLen];
    std::snprintf(b, sizeof b, "0x%04X", pdo);
    return b;
}

/**
 * @brief PDOs of the EL3356-0010 according to the device description with mapping and exclude lists.
 *
 * Mapping of the latest revision. Basis for the simulation and fallback for names without
 * SDO information.
 *
 * @return all PDOs of the latest revision, not assigned
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
 * @brief Sets assigned and the position of the entries according to the assignment order.
 * @param pdos PDO catalog
 * @param rx_assign content of 0x1C12
 * @param tx_assign content of 0x1C13
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
 * @brief Value of an assigned entry from the image.
 * @param img input or output image of the slave
 * @param e entry with position
 * @return value (bit field up to 64 bits; U64 rounded as double), 0 for gaps
 */
double entry_value(const uint8_t* img, const PdoEntry& e) {
    if (!e.loc.present || e.type == DType::None) return 0.0;
    return from_raw(e.type, get_bits(img, e.loc.bit_offset, e.loc.bit_len));
}

namespace {

/// @name Configuration area of the object dictionary (ETG.5001 Modular Device Profile)
/// @{
constexpr uint16_t kObjConfigFirst = 0x8000U;  ///< first configuration object
constexpr uint16_t kObjConfigLast = 0x8FFFU;   ///< last configuration object
/// @}
constexpr size_t kNumTextLen = 32U;  ///< buffer for a formatted number
constexpr int kHexBase = 16;         ///< base of the addresses "0xIIII" of the device description

/**
 * @brief Address as "0xIIII:SS" (key in DeviceConfig::coe and the device description).
 * @param index object index
 * @param sub subindex
 * @return address
 */
std::string obj_key(uint16_t index, uint8_t sub) {
    char b[kObjNameLen];
    std::snprintf(b, sizeof b, "0x%04X:%02X", index, sub);
    return b;
}

/**
 * @brief Number for messages (decimal comma, without superfluous digits).
 * @param v value
 * @return text
 */
std::string num_text(double v) {
    char b[kNumTextLen];
    std::snprintf(b, sizeof b, "%.10g", v);
    std::string t = b;
    std::replace(t.begin(), t.end(), '.', ',');
    return t;
}

/**
 * @brief JSON value (number or bool) as a number.
 * @param j value
 * @return number; bool as 0/1
 */
double json_number(const nlohmann::json& j) {
    return j.is_boolean() ? (j.get<bool>() ? 1.0 : 0.0) : j.get<double>();
}

/**
 * @brief Checks whether an entry is a writable terminal parameter.
 * @param e entry of the device description
 * @return true for rw entries 0x8000..0x8FFF with a numeric type, without subindex 0 and without never_write
 */
bool is_setting(const DescEntry& e) {
    if (e.index < kObjConfigFirst || e.index > kObjConfigLast || e.sub == kSubCount) return false;
    if (e.raw.value("access", "") != "rw" || e.raw.contains("never_write")) return false;
    DType t = DType::None;
    return dtype_from_name(e.type, t);
}

/**
 * @brief Checks a value against data type, limits and enumeration according to the device description.
 * @param e entry
 * @param t data type
 * @param v value (CoE raw value)
 * @return error text, empty = valid
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
 * @brief Writable terminal parameters according to the device description with the effective value.
 *
 * Value: typed field of DeviceConfig (config_key), otherwise DeviceConfig::coe, otherwise the ESI
 * default. Entries without a value and entries missing in the revision are skipped.
 *
 * @param dc terminal parameters
 * @param revision revision of the terminal, 0 = latest of the description
 * @return parameters by index and subindex
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
 * @brief Checks the terminal parameters against the device description.
 *
 * Keys in DeviceConfig::coe must be writable terminal parameters without a typed field;
 * all configured values are checked against data type, limits and enumeration.
 *
 * @param dc terminal parameters
 * @return error messages; empty = valid
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
 * @brief Write list of all terminal parameters (like a startup list in TwinCAT).
 * @param dc terminal parameters
 * @param revision revision of the terminal, 0 = latest of the description
 * @return SDO write accesses by index and subindex (also allowed in OP)
 */
std::vector<SdoWrite> settings_writes(const DeviceConfig& dc, uint32_t revision) {
    std::vector<SdoWrite> out;
    for (const auto& s : device_settings(dc, revision))
        out.push_back({s.index, s.sub, encode_value(s.type, s.value), s.name, s.type, false});
    return out;
}

/**
 * @brief ESI name of the process data variant.
 * @param variant DeviceConfig::pdo_variant
 * @return name of the AlternativeSmMapping, nullptr for an unknown variant
 */
const char* variant_name(const std::string& variant) {
    if (variant == kVariantReal) return kVariantRealName;
    if (variant == kVariantInt32) return kVariantInt32Name;
    return nullptr;
}

/**
 * @brief PDO assignment according to the device description.
 *
 * The basis is the variant (AlternativeSmMapping); PDOs from pdo_rules (0x1601 for filter
 * "PDO Filter frequency") are added when a condition is met. PDOs missing in the
 * revision are skipped.
 *
 * @param dc terminal parameters (variant, filter settings)
 * @param revision revision of the terminal, 0 = latest of the description
 * @return assignment; empty for an unknown variant
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
 * @brief Checks an assignment against the exclude lists and the revision of the device description.
 * @param a assignment
 * @param revision revision of the terminal, 0 = latest of the description
 * @return error messages; empty = valid
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
 * @brief Write list for a PDO assignment: count 0, entries, count.
 * @param sm_obj 0x1C12 or 0x1C13
 * @param pdos PDOs to assign, in order
 * @return pdos.size() + 2 SDO write accesses (only allowed in PRE-OP)
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
 * @brief Complete sequence of the transition PRE-OP -> SAFE-OP.
 * @param dc terminal parameters (variant, filter); 0x8000.. only with write_on_start
 * @param revision revision of the terminal, 0 = latest of the description
 * @return PDO assignment 0x1C12/0x1C13, then 0x8000 if applicable
 */
std::vector<SdoWrite> init_sequence(const DeviceConfig& dc, uint32_t revision) {
    const auto a = pdo_assignment(dc, revision);
    auto seq = assign_writes(kSm2Assign, a.rx);
    for (auto& w : assign_writes(kSm3Assign, a.tx)) seq.push_back(std::move(w));
    if (dc.write_on_start)
        for (auto& w : settings_writes(dc, revision)) seq.push_back(std::move(w));
    return seq;
}

/// @brief UINT16 as bytes, little endian.
std::vector<uint8_t> le_u16(uint16_t v) { return le<uint16_t>(v); }
/// @brief REAL32 as bytes, little endian.
std::vector<uint8_t> le_f32(float v) { return le<float>(v); }
/// @brief REAL32 from bytes, little endian; 0 for fewer than 4 bytes.
float f32_from_le(const std::vector<uint8_t>& d) {
    float f = 0.0F;
    if (d.size() >= sizeof(f)) std::memcpy(&f, d.data(), sizeof(f));
    return f;
}

} // namespace ew::el3356
