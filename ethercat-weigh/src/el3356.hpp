#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "sample.hpp"

namespace ew::el3356 {

// PDO-Zuordnung (Beckhoff-ESI EL3356-0010, doc/Beckhoff-ESI/Beckhoff EL33xx.xml)
constexpr uint16_t kPdoStatus = 0x1A00;     // RMB Status, 2 Byte
constexpr uint16_t kPdoValueReal = 0x1A02;  // RMB Value (Real), 4 Byte
constexpr uint16_t kPdoControl = 0x1600;    // RMB Control, 2 Byte
constexpr uint16_t kSm2Assign = 0x1C12;     // RxPDO assign
constexpr uint16_t kSm3Assign = 0x1C13;     // TxPDO assign

constexpr uint16_t kObjInputs = 0x6000;
constexpr uint16_t kObjControl = 0x7000;
constexpr uint16_t kObjSettings = 0x8000;
constexpr uint16_t kObjSmOutParam = 0x1C32;  // :20 Sync error (0x1A00 bis Revision 0x0014)
constexpr uint16_t kObjTxPdoPar = 0x1800;    // :09 TxPDO Toggle (0x1A00 bis Revision 0x0014)
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

// Datentyp eines CoE-Objekts bzw. PDO-Eintrags
enum class DType : uint8_t { None, Bool, U8, U16, U32, U64, I8, I16, I32, F32 };
const char* dtype_name(DType t);           // "bool", "u8", ... ("" für None)
bool dtype_from_name(const std::string& s, DType& t);
DType dtype_from_coe(uint16_t coe_type, uint8_t bits);  // ECT_*-Code, sonst nach Bitlänge
size_t dtype_size(DType t);                // Bytes für SDO-Zugriffe
std::vector<uint8_t> encode_value(DType t, double v);
double decode_value(DType t, const std::vector<uint8_t>& d);

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

// Simulation: Statusbits und Value (Real) in ein Eingangsabbild schreiben
void encode_inputs(uint8_t* img, const InputLayout& l, const El3356Status& s, float value_real);

// Steuerbit 0x7000:sub in El3356Control (nullptr, falls unbekannt)
bool* control_bit(El3356Control& c, uint8_t sub);

// --- PDO-Katalog (Debug) ----------------------------------------------------------
struct PdoEntry {
    MapEntry map;
    std::string name;
    DType type = DType::None;  // None: Lücke (Index 0x0000)
    EntryLoc loc;              // nur bei zugeordnetem PDO; relativ zum Slave-Abbild
};

struct PdoInfo {
    uint16_t index = 0;
    std::string name;
    bool rx = false;        // RxPDO (0x16xx, Ausgänge) oder TxPDO (0x1Axx, Eingänge)
    bool assigned = false;  // in 0x1C12/0x1C13 zugeordnet
    std::vector<PdoEntry> entries;
    std::vector<uint16_t> excludes;  // laut ESI nicht gemeinsam zuordenbar
};

// PDOs der EL3356-0010 laut ESI (Revision 0x0019) mit Mapping und Exclude-Listen
// (Simulation; Rückfall für Namen ohne SDO-Information)
std::vector<PdoInfo> known_pdos();
std::string known_pdo_name(uint16_t pdo);
// Name und Typ eines Eintrags aus der Beckhoff-Dokumentation; false, falls unbekannt
bool known_entry(uint16_t index, uint8_t sub, std::string& name, DType& type);

// Setzt assigned und loc der Einträge gemäß Zuordnungsreihenfolge (rx: 0x1C12, tx: 0x1C13)
void locate_pdos(std::vector<PdoInfo>& pdos, const std::vector<uint16_t>& rx_assign,
                 const std::vector<uint16_t>& tx_assign);
// Wert eines zugeordneten Eintrags aus dem Abbild (Bitfeld bis 64 Bit; U64 als double gerundet)
double entry_value(const uint8_t* img, const PdoEntry& e);

// --- Initialisierungssequenz -------------------------------------------------------
// CoE-Schreibliste (Mailbox) für 0x8000 aus DeviceConfig bzw. für den Übergang PRE-OP -> SAFE-OP
struct SdoWrite {
    uint16_t index;
    uint8_t sub;
    std::vector<uint8_t> data;  // little endian
    std::string name;
    DType type = DType::None;
    bool pre_op_only = false;   // nur in PRE-OP zulässig (PDO-Zuordnung)
};
std::vector<SdoWrite> settings_writes(const DeviceConfig& dc);

// Feste PDO-Zuordnung dieser Anwendung
const std::vector<uint16_t>& rx_assignment();  // 0x1C12
const std::vector<uint16_t>& tx_assignment();  // 0x1C13
std::vector<SdoWrite> assign_writes(uint16_t sm_obj, const std::vector<uint16_t>& pdos);
// Vollständige Sequenz des PO2SO-Hooks: Zuordnung, danach 0x8000 bei write_on_start
std::vector<SdoWrite> init_sequence(const DeviceConfig& dc);

std::vector<uint8_t> le_u16(uint16_t v);
std::vector<uint8_t> le_f32(float v);
float f32_from_le(const std::vector<uint8_t>& d);

} // namespace ew::el3356
