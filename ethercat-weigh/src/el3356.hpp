#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "sample.hpp"

namespace ew::el3356 {

// PDO-Zuordnung (Beckhoff-Dokumentation EL3356-00x0, Sync Manager)
constexpr uint16_t kPdoStatus = 0x1A00;     // RMB Status, 2 Byte
constexpr uint16_t kPdoValueReal = 0x1A02;  // RMB Value (Real), 4 Byte
constexpr uint16_t kPdoControl = 0x1600;    // RMB Control, 2 Byte
constexpr uint16_t kSm2Assign = 0x1C12;     // RxPDO assign
constexpr uint16_t kSm3Assign = 0x1C13;     // TxPDO assign

constexpr uint16_t kObjInputs = 0x6000;
constexpr uint16_t kObjControl = 0x7000;
constexpr uint16_t kObjSettings = 0x8000;
constexpr uint16_t kObjCommand = 0xFB00;    // RMB Command: :01 Request, :02 Status, :03 Response

// 0x8000 Subindizes
constexpr uint8_t kSubFilterMode0 = 0x11;
constexpr uint8_t kSubGain = 0x21;
constexpr uint8_t kSubTara = 0x22;
constexpr uint8_t kSubRatedOutput = 0x23;
constexpr uint8_t kSubNominalLoad = 0x24;
constexpr uint8_t kSubZeroBalance = 0x25;
constexpr uint8_t kSubGravity = 0x26;
constexpr uint8_t kSubScaleFactor = 0x27;
constexpr uint8_t kSubReferenceLoad = 0x28;

constexpr uint16_t kFilterIir8 = 9;         // 0x8000:11 Wert 9 = IIR 8

// Kommandos für 0xFB00:01 (als UINT16 geschrieben)
constexpr uint16_t kCmdNone = 0x0000;
constexpr uint16_t kCmdZeroBalance = 0x0101;  // 257: Nullabgleich
constexpr uint16_t kCmdCalibration = 0x0102;  // 258: Kalibrierung mit Reference load
constexpr uint8_t kCmdStatusDone = 0;         // 0xFB00:02 = 0: fertig ohne Fehler
constexpr uint8_t kCmdStatusBusy = 255;       // 0xFB00:02 = 255: läuft

// Bit-/Byte-Lage eines PDO-Eintrags im Prozessabbild des Slaves
struct EntryLoc {
    bool present = false;
    uint32_t bit_offset = 0;  // relativ zum Beginn des Slave-Abbilds (Ein- bzw. Ausgang)
    uint8_t bit_len = 0;
};

struct InputLayout {
    EntryLoc overrange, data_invalid, error, calibration, steady_state, sync_error, toggle;
    EntryLoc value_real;
};

struct OutputLayout {
    EntryLoc start_calibration, disable_calibration, input_freeze, sample_mode, tare;
};

// Ein Mapping-Eintrag (UINT32 aus 0x1Axx/0x16xx: index<<16 | sub<<8 | bitlen)
struct MapEntry {
    uint16_t index;
    uint8_t sub;
    uint8_t bits;
};
MapEntry decode_map_entry(uint32_t raw);

// Berechnet die Lage der Einträge aus den Mappings in Reihenfolge der PDO-Zuordnung.
InputLayout layout_inputs(const std::vector<std::vector<MapEntry>>& pdos_in_order);
OutputLayout layout_outputs(const std::vector<std::vector<MapEntry>>& pdos_in_order);

El3356Status decode_status(const uint8_t* img, const InputLayout& l);
float decode_value_real(const uint8_t* img, const InputLayout& l);
void encode_control(uint8_t* img, const OutputLayout& l, const El3356Control& c);

// CoE-Schreibliste für 0x8000 aus DeviceConfig
struct SdoWrite {
    uint16_t index;
    uint8_t sub;
    std::vector<uint8_t> data;  // little endian
    std::string name;
};
std::vector<SdoWrite> settings_writes(const DeviceConfig& dc);

std::vector<uint8_t> le_u16(uint16_t v);
std::vector<uint8_t> le_f32(float v);
float f32_from_le(const std::vector<uint8_t>& d);

} // namespace ew::el3356
