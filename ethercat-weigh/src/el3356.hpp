#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace ew {

/// Statusbits aus TxPDO 0x1A00 "RMB Status" (Objekt 0x6000).
struct El3356Status {
    bool overrange = false;               ///< 0x6000:02 Overrange
    bool data_invalid = false;            ///< 0x6000:04 Data invalid
    bool error = false;                   ///< 0x6000:07 Error
    bool calibration_in_progress = false; ///< 0x6000:08 Calibration in progress
    bool steady_state = false;            ///< 0x6000:09 Steady state
    bool sync_error = false;              ///< 0x6000:0E bzw. 0x1C32:20 Sync error
    bool txpdo_toggle = false;            ///< 0x6000:10 bzw. 0x1800:09 TxPDO Toggle
};

/// Steuerbits aus RxPDO 0x1600 "RMB Control" (Objekt 0x7000).
struct El3356Control {
    bool start_calibration = false;   ///< 0x7000:01 Start calibration
    bool disable_calibration = false; ///< 0x7000:02 Disable calibration
    bool input_freeze = false;        ///< 0x7000:03 Input freeze
    bool sample_mode = false;         ///< 0x7000:04 Sample mode
    bool tare = false;                ///< 0x7000:05 Tara
    uint16_t filter_frequency = 0;    ///< 0x7000:11 Filter frequency [0,1 Hz] (RxPDO 0x1601)
};

} // namespace ew

/**
 * @brief Objektverzeichnis, PDO-Mapping und Initialisierungssequenz der EL3356-0010.
 *
 * Referenz: Beckhoff-ESI, esi/Beckhoff EL3356-0010.xml; Gerätebeschreibung src/generated/el3356_0010.json.
 */
namespace ew::el3356 {

/// @name RxPDOs (Ausgänge)
/// @{
constexpr uint16_t kPdoControl = 0x1600U;          ///< RMB Control, 2 Byte
constexpr uint16_t kPdoFilterFrequency = 0x1601U;  ///< RMB Filter frequency
/// @}

/// @name TxPDOs (Eingänge)
/// @{
constexpr uint16_t kPdoStatus = 0x1A00U;         ///< RMB Status, 2 Byte
constexpr uint16_t kPdoValueInt = 0x1A01U;       ///< RMB Value (INT32)
constexpr uint16_t kPdoValueReal = 0x1A02U;      ///< RMB Value (Real), 4 Byte
constexpr uint16_t kPdoTimestamp = 0x1A03U;      ///< RMB Timestamp
constexpr uint16_t kPdoAiStandardCh1 = 0x1A04U;  ///< AI Standard Channel 1
constexpr uint16_t kPdoAiCompactCh1 = 0x1A05U;   ///< AI Compact Channel 1
constexpr uint16_t kPdoAiStandardCh2 = 0x1A06U;  ///< AI Standard Channel 2
constexpr uint16_t kPdoAiCompactCh2 = 0x1A07U;   ///< AI Compact Channel 2
/// @}

/// @name Bereiche der PDO-Mapping-Objekte (CoE: 0x1600+n RxPDO, 0x1A00+n TxPDO)
/// @{
constexpr uint16_t kPdoRxFirst = kPdoControl;  ///< erstes RxPDO-Mapping-Objekt
constexpr uint16_t kPdoTxFirst = kPdoStatus;   ///< erstes TxPDO-Mapping-Objekt
constexpr uint16_t kPdoRangeSize = 8U;         ///< geprüfte Objekte je Richtung (0x1600..0x1607, 0x1A00..0x1A07)
constexpr uint16_t kPdoGroupMask = 0xFF00U;    ///< Maske für den Bereich 0x16xx bzw. 0x1Axx
/// @}

/// @name PDO-Zuordnung
/// @{
constexpr uint16_t kSm2Assign = 0x1C12U;  ///< RxPDO assign (SM2)
constexpr uint16_t kSm3Assign = 0x1C13U;  ///< TxPDO assign (SM3)
constexpr uint8_t kSubCount = 0x00U;      ///< Subindex 0: Anzahl der Einträge (Zuordnung, Mapping)
constexpr uint8_t kSubFirstEntry = 0x01U; ///< erster Eintrag hinter Subindex 0
/// @}

/// @name Objekte
/// @{
constexpr uint16_t kObjGap = 0x0000U;         ///< Index einer Lücke im Mapping
constexpr uint16_t kObjInputs = 0x6000U;      ///< RMB Inputs
constexpr uint16_t kObjAiInputsCh1 = 0x6010U; ///< AI Inputs Kanal 1
constexpr uint16_t kObjAiInputsCh2 = 0x6020U; ///< AI Inputs Kanal 2
constexpr uint16_t kObjControl = 0x7000U;     ///< RMB Outputs
constexpr uint16_t kObjSettings = 0x8000U;    ///< RMB Settings
constexpr uint16_t kObjSmOutParam = 0x1C32U;  ///< SM output parameter (Sync error bis Revision 0x0014)
constexpr uint16_t kObjTxPdoPar = 0x1800U;    ///< RMB TxPDO-Par Status (Toggle bis Revision 0x0014)
constexpr uint16_t kObjCommand = 0xFB00U;     ///< RMB Command
constexpr uint16_t kObjInfo = 0x9000U;        ///< RMB Info data
constexpr uint8_t kSubInfoMvV = 0x11U;        ///< 0x9000:11 mV/V
constexpr uint16_t kObjDiag = 0xA000U;        ///< RMB Diag data
/// @}

/// @name Subindizes von 0x6000 (RMB Inputs)
/// @{
constexpr uint8_t kSubInOverrange = 0x02U;      ///< Overrange
constexpr uint8_t kSubInDataInvalid = 0x04U;    ///< Data invalid
constexpr uint8_t kSubInError = 0x07U;          ///< Error
constexpr uint8_t kSubInCalibration = 0x08U;    ///< Calibration in progress
constexpr uint8_t kSubInSteadyState = 0x09U;    ///< Steady state
constexpr uint8_t kSubInSyncError = 0x0EU;      ///< Sync error (ab Revision 0x0015)
constexpr uint8_t kSubInToggle = 0x10U;         ///< TxPDO Toggle (ab Revision 0x0015)
constexpr uint8_t kSubInValue = 0x11U;          ///< Value (DINT)
constexpr uint8_t kSubInValueReal = 0x12U;      ///< Value (Real)
constexpr uint8_t kSubInTimestamp = 0x13U;      ///< Timestamp (ULINT)
/// @}

/// @name Subindizes von 0x6010/0x6020 (AI Inputs)
/// @{
constexpr uint8_t kSubAiUnderrange = 0x01U;  ///< Underrange
constexpr uint8_t kSubAiOverrange = 0x02U;   ///< Overrange
constexpr uint8_t kSubAiError = 0x07U;       ///< Error
constexpr uint8_t kSubAiToggle = 0x10U;      ///< TxPDO Toggle
constexpr uint8_t kSubAiValue = 0x11U;       ///< Value (DINT)
/// @}

/// @name Subindizes der Objekte vor Revision 0x0015
/// @{
constexpr uint8_t kSubSmOutSyncError = 0x20U;  ///< 0x1C32:20 Sync error
constexpr uint8_t kSubTxPdoParToggle = 0x09U;  ///< 0x1800:09 TxPDO Toggle
/// @}

/// @name Subindizes von 0x7000 (RMB Outputs)
/// @{
constexpr uint8_t kSubCtrlStartCalibration = 0x01U;    ///< Start calibration
constexpr uint8_t kSubCtrlDisableCalibration = 0x02U;  ///< Disable calibration
constexpr uint8_t kSubCtrlInputFreeze = 0x03U;         ///< Input freeze
constexpr uint8_t kSubCtrlSampleMode = 0x04U;          ///< Sample mode
constexpr uint8_t kSubCtrlTare = 0x05U;                ///< Tara
constexpr uint8_t kSubCtrlFilterFrequency = 0x11U;     ///< Filter frequency (UINT)
/// @}

/// @name Subindizes von 0x8000 (RMB Settings)
/// @{
constexpr uint8_t kSubMode0EnableFilter = 0x01U;     ///< Mode0 enable filter
constexpr uint8_t kSubMode0EnableAverager = 0x03U;   ///< Mode0 enable averager
constexpr uint8_t kSubFilterMode0 = 0x11U;           ///< Mode0 filter settings
constexpr uint8_t kSubGain = 0x21U;                  ///< Gain
constexpr uint8_t kSubTara = 0x22U;                  ///< Tara
constexpr uint8_t kSubRatedOutput = 0x23U;           ///< Rated output [mV/V]
constexpr uint8_t kSubNominalLoad = 0x24U;           ///< Nominal load
constexpr uint8_t kSubZeroBalance = 0x25U;           ///< Zero balance [mV/V]
constexpr uint8_t kSubGravity = 0x26U;               ///< Gravity of earth
constexpr uint8_t kSubScaleFactor = 0x27U;           ///< Scale factor
constexpr uint8_t kSubReferenceLoad = 0x28U;         ///< Reference load
constexpr uint8_t kSubSteadyStateWindow = 0x29U;     ///< Steady state window [ms]
constexpr uint8_t kSubSteadyStateTolerance = 0x2AU;  ///< Steady state tolerance
constexpr uint8_t kSubCalibrationInterval = 0x31U;   ///< Calibration interval
/// @}

constexpr uint16_t kFilterIir8 = 9U;           ///< 0x8000:11 Wert 9 = IIR8
constexpr uint16_t kFilterPdoFrequency = 11U;  ///< 0x8000:11 letzter Wert: PDO Filter frequency (0x7000:11)

/// @name Subindizes von 0xFB00 (RMB Command)
/// @{
constexpr uint8_t kSubCmdRequest = 0x01U;   ///< Request (Kommando, als UINT16 geschrieben)
constexpr uint8_t kSubCmdStatus = 0x02U;    ///< Status (USINT)
constexpr uint8_t kSubCmdResponse = 0x03U;  ///< Response
/// @}

/// @name Restore default parameters (0x1011)
/// @{
constexpr uint16_t kObjRestore = 0x1011U;            ///< Restore default parameters
constexpr uint8_t kSubRestoreAll = 0x01U;            ///< alle Backup-Objekte
constexpr uint32_t kRestoreSignature = 0x64616F6CU;  ///< "load" (Doku Kap. 4.10.1)
/// @}

/// @name Kommandos für 0xFB00:01 und Status 0xFB00:02
/// @{
constexpr uint16_t kCmdNone = 0x0000U;         ///< kein Kommando
constexpr uint16_t kCmdTareTemporary = 0x0001U;  ///< Tarierung, nicht im EEPROM (Doku Kap. 4.4.4)
constexpr uint16_t kCmdTarePermanent = 0x0002U;  ///< Tarierung, im EEPROM (Doku Kap. 4.4.4)
constexpr uint16_t kCmdZeroBalance = 0x0101U;  ///< 257: Nullabgleich
constexpr uint16_t kCmdCalibration = 0x0102U;  ///< 258: Kalibrierung mit Reference load
constexpr uint8_t kCmdStatusDone = 0U;         ///< 0xFB00:02 = 0: fertig ohne Fehler
constexpr uint8_t kCmdStatusBusy = 255U;       ///< 0xFB00:02 = 255: läuft
/// @}

/// @name Bitlängen von PDO-Einträgen
/// @{
constexpr uint8_t kBitsBool = 1U;    ///< BOOL
constexpr uint8_t kBitsUint = 16U;   ///< UINT
constexpr uint8_t kBitsDint = 32U;   ///< DINT, REAL
constexpr uint8_t kBitsUlint = 64U;  ///< ULINT
/// @}

/// @name Datentypcodes der SDO-Information (ETG.1000.6, ECT_*)
/// @{
constexpr uint16_t kCoeUnknown = 0x0000U;     ///< kein Code bekannt
constexpr uint16_t kCoeBoolean = 0x0001U;     ///< BOOLEAN
constexpr uint16_t kCoeInteger8 = 0x0002U;    ///< INTEGER8
constexpr uint16_t kCoeInteger16 = 0x0003U;   ///< INTEGER16
constexpr uint16_t kCoeInteger32 = 0x0004U;   ///< INTEGER32
constexpr uint16_t kCoeUnsigned8 = 0x0005U;   ///< UNSIGNED8
constexpr uint16_t kCoeUnsigned16 = 0x0006U;  ///< UNSIGNED16
constexpr uint16_t kCoeUnsigned32 = 0x0007U;  ///< UNSIGNED32
constexpr uint16_t kCoeReal32 = 0x0008U;      ///< REAL32
constexpr uint16_t kCoeUnsigned64 = 0x001BU;  ///< UNSIGNED64
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
/// Alle Datentypen mit Wert (ohne DType::None), z. B. für Auswahllisten
inline constexpr DType kValueTypes[] = {DType::Bool, DType::U8,  DType::U16, DType::U32, DType::U64,
                                        DType::I8,   DType::I16, DType::I32, DType::F32};
const char* dtype_name(DType t);
bool dtype_from_name(const std::string& s, DType& t);
DType dtype_from_coe(uint16_t coe, uint8_t bits);
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
    EntryLoc value_int;     ///< 0x6000:11 Value (INT32)
    EntryLoc value_real;    ///< 0x6000:12
};

/// Lage der Steuerbits 0x7000.
struct OutputLayout {
    EntryLoc start_calibration;    ///< 0x7000:01
    EntryLoc disable_calibration;  ///< 0x7000:02
    EntryLoc input_freeze;         ///< 0x7000:03
    EntryLoc sample_mode;          ///< 0x7000:04
    EntryLoc tare;                 ///< 0x7000:05
    EntryLoc filter_frequency;     ///< 0x7000:11 (RxPDO 0x1601)
};

/// Ein Mapping-Eintrag (UINT32 aus 0x1Axx/0x16xx: index<<16 | sub<<8 | bitlen).
struct MapEntry {
    uint16_t index;  ///< Objektindex, 0 = Lücke
    uint8_t sub;     ///< Subindex
    uint8_t bits;    ///< Länge in Bit
};
MapEntry decode_map_entry(uint32_t raw);
uint32_t encode_map_entry(const MapEntry& e);
bool is_pdo_mapping(uint16_t index);
size_t bytes_for_bits(uint32_t bits);

InputLayout layout_inputs(const std::vector<std::vector<MapEntry>>& pdos_in_order);
OutputLayout layout_outputs(const std::vector<std::vector<MapEntry>>& pdos_in_order);

El3356Status decode_status(const uint8_t* img, const InputLayout& l);
float decode_value_real(const uint8_t* img, const InputLayout& l);
double decode_measurement(const uint8_t* img, const InputLayout& l);
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

/// Schreibbarer Klemmenparameter (0x8000..0x8FFF, rw) mit dem wirksamen Wert.
struct DeviceSetting {
    uint16_t index = 0;        ///< Objektindex
    uint8_t sub = 0;           ///< Subindex
    std::string name;          ///< Name laut ESI
    std::string key;           ///< Konfigurationspfad: "device.<config_key>" bzw. "device.coe.0xIIII:SS"
    DType type = DType::None;  ///< Datentyp
    double value = 0.0;        ///< CoE-Rohwert
    bool configured = false;   ///< aus der Konfiguration; false = Default der ESI
};
std::vector<DeviceSetting> device_settings(const DeviceConfig& dc, uint32_t revision = 0U);
std::vector<std::string> validate_device(const DeviceConfig& dc);
std::vector<SdoWrite> settings_writes(const DeviceConfig& dc, uint32_t revision = 0U);

/// @name Prozessdaten-Varianten (DeviceConfig::pdo_variant -> AlternativeSmMapping der ESI)
/// @{
constexpr const char* kVariantReal = "real";             ///< Standard (REAL): 0x1A02
constexpr const char* kVariantInt32 = "int32";           ///< Standard (INT32): 0x1A01
constexpr const char* kVariantRealName = "Standard (REAL)";    ///< Name in der ESI
constexpr const char* kVariantInt32Name = "Standard (INT32)";  ///< Name in der ESI
/// @}

/// PDO-Zuordnung der Sync-Manager.
struct PdoAssignment {
    std::vector<uint16_t> rx;  ///< 0x1C12 (SM2)
    std::vector<uint16_t> tx;  ///< 0x1C13 (SM3)
};
const char* variant_name(const std::string& variant);
PdoAssignment pdo_assignment(const DeviceConfig& dc, uint32_t revision = 0U);
std::vector<std::string> check_assignment(const PdoAssignment& a, uint32_t revision = 0U);
std::vector<SdoWrite> assign_writes(uint16_t sm_obj, const std::vector<uint16_t>& pdos);
std::vector<SdoWrite> init_sequence(const DeviceConfig& dc, uint32_t revision = 0U);
/// @}

std::vector<uint8_t> le_u16(uint16_t v);
std::vector<uint8_t> le_f32(float v);
float f32_from_le(const std::vector<uint8_t>& d);

} // namespace ew::el3356
