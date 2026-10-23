#include "el3356.hpp"

#include <cstring>

namespace ew::el3356 {

MapEntry decode_map_entry(uint32_t raw) {
    return {static_cast<uint16_t>(raw >> 16), static_cast<uint8_t>((raw >> 8) & 0xFF),
            static_cast<uint8_t>(raw & 0xFF)};
}

namespace {

template <typename F>
void walk(const std::vector<std::vector<MapEntry>>& pdos, F&& on_entry) {
    uint32_t off = 0;
    for (const auto& pdo : pdos)
        for (const auto& e : pdo) {
            on_entry(e, off);
            off += e.bits;
        }
}

bool get_bit(const uint8_t* img, const EntryLoc& l) {
    if (!l.present) return false;
    return (img[l.bit_offset / 8] >> (l.bit_offset % 8)) & 1u;
}

void set_bit(uint8_t* img, const EntryLoc& l, bool v) {
    if (!l.present) return;
    const uint8_t mask = static_cast<uint8_t>(1u << (l.bit_offset % 8));
    if (v)
        img[l.bit_offset / 8] |= mask;
    else
        img[l.bit_offset / 8] &= static_cast<uint8_t>(~mask);
}

template <typename T>
std::vector<uint8_t> le(T v) {
    std::vector<uint8_t> out(sizeof(T));
    std::memcpy(out.data(), &v, sizeof(T));  // Zielplattformen sind little endian
    return out;
}

} // namespace

InputLayout layout_inputs(const std::vector<std::vector<MapEntry>>& pdos) {
    InputLayout l;
    walk(pdos, [&](const MapEntry& e, uint32_t off) {
        if (e.index != kObjInputs) return;
        EntryLoc loc{true, off, e.bits};
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

float decode_value_real(const uint8_t* img, const InputLayout& l) {
    float f = 0.0f;
    if (!l.value_real.present || l.value_real.bit_len != 32 || (l.value_real.bit_offset % 8) != 0)
        return f;
    std::memcpy(&f, img + l.value_real.bit_offset / 8, sizeof(f));
    return f;
}

void encode_control(uint8_t* img, const OutputLayout& l, const El3356Control& c) {
    set_bit(img, l.start_calibration, c.start_calibration);
    set_bit(img, l.disable_calibration, c.disable_calibration);
    set_bit(img, l.input_freeze, c.input_freeze);
    set_bit(img, l.sample_mode, c.sample_mode);
    set_bit(img, l.tare, c.tare);
}

std::vector<SdoWrite> settings_writes(const DeviceConfig& dc) {
    return {
        {kObjSettings, 0x01, le<uint8_t>(dc.mode0_enable_filter), "Mode0 enable filter"},
        {kObjSettings, 0x03, le<uint8_t>(dc.mode0_enable_averager), "Mode0 enable averager"},
        {kObjSettings, 0x11, le<uint16_t>(dc.mode0_filter_settings), "Mode0 filter settings"},
        {kObjSettings, 0x23, le<float>(static_cast<float>(dc.rated_output_mv_v)), "Rated output"},
        {kObjSettings, 0x24, le<float>(static_cast<float>(dc.nominal_load)), "Nominal load"},
        {kObjSettings, 0x25, le<float>(static_cast<float>(dc.zero_balance)), "Zero balance"},
        {kObjSettings, 0x26, le<float>(static_cast<float>(dc.gravity_of_earth)), "Gravity of earth"},
        {kObjSettings, 0x27, le<float>(static_cast<float>(dc.scale_factor)), "Scale factor"},
        {kObjSettings, 0x28, le<float>(static_cast<float>(dc.reference_load)), "Reference load"},
        {kObjSettings, 0x29, le<uint16_t>(dc.steady_state_window), "Steady state window"},
        {kObjSettings, 0x2A, le<uint32_t>(dc.steady_state_tolerance), "Steady state tolerance"},
        {kObjSettings, 0x31, le<uint16_t>(dc.calibration_interval), "Calibration interval"},
    };
}

std::vector<uint8_t> le_u16(uint16_t v) { return le<uint16_t>(v); }
std::vector<uint8_t> le_f32(float v) { return le<float>(v); }
float f32_from_le(const std::vector<uint8_t>& d) {
    float f = 0.0f;
    if (d.size() >= sizeof(f)) std::memcpy(&f, d.data(), sizeof(f));
    return f;
}

} // namespace ew::el3356
