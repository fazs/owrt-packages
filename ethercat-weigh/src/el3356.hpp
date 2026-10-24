#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "sample.hpp"

/**
 * @brief Objektverzeichnis, PDO-Mapping und Initialisierungssequenz der EL3356-0010.
 *
 * Referenz: Beckhoff-ESI, misc/Beckhoff-ESI/Beckhoff EL33xx.xml.
 */
namespace ew::el3356 {

/// @name PDOs und PDO-Zuordnung
/// @{
constexpr uint16_t kPdoStatus = 0x1A00;     ///< TxPDO RMB Status, 2 Byte
constexpr uint16_t kPdoValueReal = 0x1A02;  ///< TxPDO RMB Value (Real), 4 Byte
constexpr uint16_t kPdoControl = 0x1600;    ///< RxPDO RMB Control, 2 Byte
constexpr uint16_t kSm2Assign = 0x1C12;     ///< RxPDO assign (SM2)
constexpr uint16_t kSm3Assign = 0x1C13;     ///< TxPDO assign (SM3)
/// @}

/// @name Objekte
/// @{
constexpr uint16_t kObjInputs = 0x6000;      ///< RMB Inputs
constexpr uint16_t kObjControl = 0x7000;     ///< RMB Outputs
constexpr uint16_t kObjSettings = 0x8000;    ///< RMB Settings
constexpr uint16_t kObjSmOutParam = 0x1C32;  ///< :20 Sync error (0x1A00 bis Revision 0x0014)
constexpr uint16_t kObjTxPdoPar = 0x1800;    ///< :09 TxPDO Toggle (0x1A00 bis Revision 0x0014)
constexpr uint16_t kObjCommand = 0xFB00;     ///< RMB Command: :01 Request, :02 Status, :03 Response
/// @}

/// @name Subindizes von 0x8000
/// @{
constexpr uint8_t kSubFilterMode0 = 0x11;    ///< Mode0 filter settings
constexpr uint8_t kSubGain = 0x21;           ///< Gain
constexpr uint8_t kSubTara = 0x22;           ///< Tara
constexpr uint8_t kSubRatedOutput = 0x23;    ///< Rated output [mV/V]
constexpr uint8_t kSubNominalLoad = 0x24;    ///< Nominal load
constexpr uint8_t kSubZeroBalance = 0x25;    ///< Zero balance [mV/V]
constexpr uint8_t kSubGravity = 0x26;        ///< Gravity of earth
constexpr uint8_t kSubScaleFactor = 0x27;    ///< Scale factor
constexpr uint8_t kSubReferenceLoad = 0x28;  ///< Reference load
/// @}

constexpr uint16_t kFilterIir8 = 9;  ///< 0x8000:11 Wert 9 = IIR8

/// @name Kommandos für 0xFB00:01 (als UINT16 geschrieben) und Status 0xFB00:02
/// @{
constexpr uint16_t kCmdNone = 0x0000;         ///< kein Kommando
constexpr uint16_t kCmdZeroBalance = 0x0101;  ///< 257: Nullabgleich
constexpr uint16_t kCmdCalibration = 0x0102;  ///< 258: Kalibrierung mit Reference load
constexpr uint8_t kCmdStatusDone = 0;         ///< 0xFB00:02 = 0: fertig ohne Fehler
constexpr uint8_t kCmdStatusBusy = 255;       ///< 0xFB00:02 = 255: läuft
/// @}

/// Datentyp eines CoE-Objekts bzw. PDO-Eintrags.
enum class DType : uint8_t {
    None,  ///< kein Wert (Lücke im Mapping)
    Bool,  ///< BOOL
    U8,    ///< USINT
    U16,   ///< UINT
    U32,   ///< UDINT
    U64,   ///< ULINT
    I8,    ///< SINT
    I16,   ///< INT
    I32,   ///< DINT
    F32    ///< REAL
};
const char* dtype_name(DType t);
bool dtype_from_name(const std::string& s, DType& t);
DType dtype_from_coe(uint16_t coe_type, uint8_t bits);
size_t dtype_size(DType t);
std::vector<uint8_t> encode_value(DType t, double v);
double decode_value(DType t, const std::vector<uint8_t>& d);

/// Bit-Lage eines PDO-Eintrags im Prozessabbild des Slaves.
struct EntryLoc {
    bool present = false;     ///< Eintrag ist gemappt
    uint32_t bit_offset = 0;  ///< relativ zum Beginn des Slave-Abbilds (Ein- bzw. Ausgang)
    uint8_t bit_len = 0;      ///< Länge in Bit
};

/// Lage der ausgewerteten Eingänge.
struct InputLayout {
    EntryLoc overrange;     ///< 0x6000:02
    EntryLoc data_invalid;  ///< 0x6000:04
    EntryLoc error;         ///< 0x6000:07
    EntryLoc calibration;   ///< 0x6000:08
    EntryLoc steady_state;  ///< 0x6000:09
    EntryLoc sync_error;    ///< 0x6000:0E bzw. 0x1C32:20
    EntryLoc toggle;        ///< 0x6000:10 bzw. 0x1800:09
    EntryLoc value_real;    ///< 0x6000:12
};

/// Lage der Steuerbits 0x7000.
struct OutputLayout {
    EntryLoc start_calibration;    ///< 0x7000:01
    EntryLoc disable_calibration;  ///< 0x7000:02
    EntryLoc input_freeze;         ///< 0x7000:03
    EntryLoc sample_mode;          ///< 0x7000:04
    EntryLoc tare;                 ///< 0x7000:05
};

/// Ein Mapping-Eintrag (UINT32 aus 0x1Axx/0x16xx: index<<16 | sub<<8 | bitlen).
struct MapEntry {
    uint16_t index;  ///< Objektindex, 0 = Lücke
    uint8_t sub;     ///< Subindex
    uint8_t bits;    ///< Länge in Bit
};
MapEntry decode_map_entry(uint32_t raw);

InputLayout layout_inputs(const std::vector<std::vector<MapEntry>>& pdos_in_order);
OutputLayout layout_outputs(const std::vector<std::vector<MapEntry>>& pdos_in_order);

El3356Status decode_status(const uint8_t* img, const InputLayout& l);
float decode_value_real(const uint8_t* img, const InputLayout& l);
void encode_control(uint8_t* img, const OutputLayout& l, const El3356Control& c);
void encode_inputs(uint8_t* img, const InputLayout& l, const El3356Status& s, float value_real);
bool* control_bit(El3356Control& c, uint8_t sub);

/// @name PDO-Katalog (Debug)
/// @{

/// Eintrag eines PDOs mit Name, Typ und Lage.
struct PdoEntry {
    MapEntry map;              ///< Mapping-Eintrag
    std::string name;          ///< Name (SDO-Information oder ESI)
    DType type = DType::None;  ///< None: Lücke (Index 0x0000)
    EntryLoc loc;              ///< nur bei zugeordnetem PDO; relativ zum Slave-Abbild
};

/// Ein PDO der Klemme.
struct PdoInfo {
    uint16_t index = 0;              ///< 0x16xx bzw. 0x1Axx
    std::string name;                ///< Name des PDOs
    bool rx = false;                 ///< RxPDO (0x16xx, Ausgänge) oder TxPDO (0x1Axx, Eingänge)
    bool assigned = false;           ///< in 0x1C12/0x1C13 zugeordnet
    std::vector<PdoEntry> entries;   ///< Einträge in Mapping-Reihenfolge
    std::vector<uint16_t> excludes;  ///< laut ESI nicht gemeinsam zuordenbar
};

std::vector<PdoInfo> known_pdos();
std::string known_pdo_name(uint16_t pdo);
bool known_entry(uint16_t index, uint8_t sub, std::string& name, DType& type);
void locate_pdos(std::vector<PdoInfo>& pdos, const std::vector<uint16_t>& rx_assign,
                 const std::vector<uint16_t>& tx_assign);
double entry_value(const uint8_t* img, const PdoEntry& e);
/// @}

/// @name Initialisierungssequenz
/// @{

/// Ein SDO-Schreibzugriff (Mailbox).
struct SdoWrite {
    uint16_t index;             ///< Objektindex
    uint8_t sub;                ///< Subindex
    std::vector<uint8_t> data;  ///< Daten, little endian
    std::string name;           ///< Beschreibung für Log und Debug-Dialog
    DType type = DType::None;   ///< Datentyp von data
    bool pre_op_only = false;   ///< nur in PRE-OP zulässig (PDO-Zuordnung)
};
std::vector<SdoWrite> settings_writes(const DeviceConfig& dc);

const std::vector<uint16_t>& rx_assignment();
const std::vector<uint16_t>& tx_assignment();
std::vector<SdoWrite> assign_writes(uint16_t sm_obj, const std::vector<uint16_t>& pdos);
std::vector<SdoWrite> init_sequence(const DeviceConfig& dc);
/// @}

std::vector<uint8_t> le_u16(uint16_t v);
std::vector<uint8_t> le_f32(float v);
float f32_from_le(const std::vector<uint8_t>& d);

} // namespace ew::el3356
