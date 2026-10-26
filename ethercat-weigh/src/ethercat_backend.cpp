#include "ethercat_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "device_description.hpp"
#include "ethercat.h"
#include "log.hpp"

namespace ew {

namespace {

/// Context for the PRE-OP -> SAFE-OP hook (SOEM calls a free function)
const Config* g_cfg = nullptr;
/// Revision of the terminal for the device description (0 = latest), for the PO2SO hook
uint32_t g_revision = 0U;

constexpr uint16_t kStateMask = 0x0FU;     ///< state bits without EC_STATE_ERROR (ETG.1000.6 AL status)
constexpr int kSafeOpTimeoutFactor = 4;    ///< waiting time for SAFE-OP: multiple of EC_TIMEOUTSTATE
/// WKC contribution per output slave: LRW counts write and read (SOEM simple_test)
constexpr int kOutputWkcFactor = 2;
constexpr int kOpRetries = 200;            ///< attempts until OP (one process data cycle each)
constexpr int kOpCheckTimeoutUs = 50000;   ///< waiting time per state check for OP [µs]
constexpr double kCheckIntervalS = 0.5;    ///< minimum interval of the state checks on WKC errors [s]
constexpr int kRecoverTimeoutUs = 500;     ///< ec_reconfig_slave/ec_recover_slave [µs]
constexpr size_t kMsgLen = 96U;            ///< buffer for SDO error messages
constexpr size_t kStateMsgLen = 256U;      ///< buffer per slave in slave_states()
constexpr size_t kObjNameLen = sizeof("0xIIII:SS");  ///< buffer for "0xIIII:SS" incl. null byte

/**
 * @brief Reads an SDO of fixed size.
 * @param slave slave number
 * @param idx index
 * @param sub subindex
 * @param[out] v value (T{} on error)
 * @return true on success
 */
template <typename T>
bool sdo_read_v(uint16_t slave, uint16_t idx, uint8_t sub, T& v) {
    int size = sizeof(T);
    v = T{};
    return ec_SDOread(slave, idx, sub, FALSE, &size, &v, EC_TIMEOUTRXM) > 0;
}

/**
 * @brief Writes the terminal parameters; aborts at the first error.
 * @param slave slave number
 * @param dc terminal parameters
 * @param revision revision for the device description, 0 = latest
 * @param[out] err error text with object and name
 * @return true on success
 */
bool write_settings(uint16_t slave, const DeviceConfig& dc, uint32_t revision, std::string& err) {
    for (const auto& w : el3356::settings_writes(dc, revision)) {
        auto data = w.data;
        if (ec_SDOwrite(slave, w.index, w.sub, FALSE, static_cast<int>(data.size()), data.data(),
                        EC_TIMEOUTRXM) <= 0) {
            char b[kMsgLen];
            std::snprintf(b, sizeof b, "SDO-Write 0x%04X:%02X (%s) fehlgeschlagen", w.index, w.sub,
                          w.name.c_str());
            err = b;
            return false;
        }
    }
    return true;
}

/**
 * @brief SOEM hook PRE-OP -> SAFE-OP: sends the initialization sequence.
 *
 * PDO assignment and CoE parameters according to el3356::init_sequence(); failed
 * steps are logged, the remaining ones are sent anyway.
 *
 * @param slave slave number
 * @return always 1 (SOEM continues the transition)
 */
int po2so_hook(uint16 slave) {
    if (!g_cfg) return 1;
    int failed = 0;
    for (const auto& w : el3356::init_sequence(g_cfg->device, g_revision)) {
        auto data = w.data;
        if (ec_SDOwrite(slave, w.index, w.sub, FALSE, static_cast<int>(data.size()), data.data(),
                        EC_TIMEOUTRXM) <= 0) {
            log_error("SDO-Write 0x%04X:%02X (%s) fehlgeschlagen", w.index, w.sub, w.name.c_str());
            ++failed;
        }
    }
    if (failed == 0) log_info("Initialisierungssequenz geschrieben (PDO-Zuordnung%s)",
                              g_cfg->device.write_on_start ? ", Klemmenparameter" : "");
    return 1;
}

/**
 * @brief Reads the mapping of a PDO (0x16xx/0x1Axx).
 * @param slave slave number
 * @param pdo PDO index
 * @param[out] out entries
 * @return false if the object does not exist or cannot be read
 */
bool read_pdo_mapping(uint16_t slave, uint16_t pdo, std::vector<el3356::MapEntry>& out) {
    uint8_t n = 0;
    if (!sdo_read_v(slave, pdo, 0, n)) return false;
    out.clear();
    for (uint8_t i = 1; i <= n; ++i) {
        uint32_t raw = 0;
        if (!sdo_read_v(slave, pdo, i, raw)) return false;
        out.push_back(el3356::decode_map_entry(raw));
    }
    return true;
}

/**
 * @brief Reads a PDO assignment (0x1C12/0x1C13) including mappings.
 * @param slave slave number
 * @param sm_obj 0x1C12 or 0x1C13
 * @param[out] out mappings in assignment order
 * @param[out] indices assigned PDO indices
 * @return true on success
 */
bool read_assigned(uint16_t slave, uint16_t sm_obj, std::vector<std::vector<el3356::MapEntry>>& out,
                   std::vector<uint16_t>& indices) {
    uint8_t n = 0;
    if (!sdo_read_v(slave, sm_obj, 0, n)) return false;
    out.clear();
    indices.clear();
    for (uint8_t i = 1; i <= n; ++i) {
        uint16_t pdo = 0;
        if (!sdo_read_v(slave, sm_obj, i, pdo)) return false;
        indices.push_back(pdo);
        std::vector<el3356::MapEntry> m;
        if (!read_pdo_mapping(slave, pdo, m)) return false;
        out.push_back(std::move(m));
    }
    return true;
}

/**
 * @brief Name of an EtherCAT state (without error bit).
 * @param st state from ec_slave[].state
 * @return "INIT", "PRE-OP", "BOOT", "SAFE-OP", "OP" or "NONE"
 */
const char* state_name(uint16_t st) {
    switch (st & kStateMask) {
    case EC_STATE_INIT: return "INIT";
    case EC_STATE_PRE_OP: return "PRE-OP";
    case EC_STATE_BOOT: return "BOOT";
    case EC_STATE_SAFE_OP: return "SAFE-OP";
    case EC_STATE_OPERATIONAL: return "OP";
    default: return "NONE";
    }
}

/**
 * @brief State, error bit and AL status code of all slaves that have not reached @p expected.
 *
 * Precondition: ec_readstate() was called immediately before.
 *
 * @param expected expected state
 * @return description for the error message
 */
std::string slave_states(uint16_t expected) {
    std::string out;
    for (int i = 1; i <= ec_slavecount; ++i) {
        const ec_slavet& sl = ec_slave[i];
        if (sl.state == expected) continue;
        char b[kStateMsgLen];
        std::snprintf(b, sizeof b, "%sSlave %d %s: %s%s, AL-Status 0x%04X (%s)",
                      out.empty() ? "" : "; ", i, sl.name, state_name(sl.state),
                      (sl.state & EC_STATE_ERROR) ? "+ERROR" : "", sl.ALstatuscode,
                      ec_ALstatuscode2string(sl.ALstatuscode));
        out += b;
    }
    return out.empty() ? "Gruppenzustand abweichend" : out;
}

/// @brief Shifts a mapped position by @p bits (start bit of the slave in the IOmap).
void shift(el3356::EntryLoc& l, uint32_t bits) {
    if (l.present) l.bit_offset += bits;
}

/// Names and data types via SDO information (if the terminal supports it).
struct SdoInfo {
    uint16_t slave;
    bool supported;
    ec_ODlistt od{};
    ec_OElistt oe{};

    /**
     * @brief Name of an object (ODdescription).
     * @param index object index
     * @param[out] name name
     * @return false without SDO information or on error
     */
    bool object_name(uint16_t index, std::string& name) {
        if (!supported) return false;
        od.Slave = slave;
        od.Entries = 1;
        od.Index[0] = index;
        if (ec_readODdescription(0, &od) <= 0 || od.Name[0][0] == 0) return false;
        name = od.Name[0];
        return true;
    }
    /**
     * @brief Name and data type of an entry (OEsingle).
     * @param index object index
     * @param sub subindex
     * @param[out] name name
     * @param[out] type CoE data type code (ECT_*)
     * @return false without SDO information or on error
     */
    bool entry(uint16_t index, uint8_t sub, std::string& name, uint16_t& type) {
        if (!supported) return false;
        od.Slave = slave;
        od.Entries = 1;
        od.Index[0] = index;
        if (ec_readOEsingle(0, sub, &od, &oe) <= 0 || oe.Name[sub][0] == 0) return false;
        name = oe.Name[sub];
        type = oe.DataType[sub];
        return true;
    }
};

} // namespace

/**
 * @brief Initializes SOEM, finds the EL3356 and brings all slaves to OP.
 *
 * The initialization sequence runs in the PO2SO hook; after SAFE-OP, mapping and
 * PDO catalog are read.
 *
 * @param cfg configuration (interface, slave name, terminal parameters)
 * @param[out] err cause on false, incl. state and AL status of the slaves
 * @return true in OP
 */
bool EthercatBackend::open(const Config& cfg, std::string& err) {
    cfg_ = cfg;
    g_cfg = &cfg_;
    wkc_fault_cycles_ = cfg.ethercat.wkc_fault_cycles;

    if (ec_init(cfg.ethercat.interface.c_str()) <= 0) {
        err = "ec_init auf '" + cfg.ethercat.interface + "' fehlgeschlagen (root/CAP_NET_RAW?)";
        return false;
    }
    opened_ = true;

    if (ec_config_init(FALSE) <= 0) {
        err = "Keine EtherCAT-Slaves gefunden";
        return false;
    }
    slave_ = 0;
    for (int i = 1; i <= ec_slavecount; ++i) {
        log_info("Slave %d: %s (Vendor 0x%08X, Product 0x%08X)", i, ec_slave[i].name,
                 ec_slave[i].eep_man, ec_slave[i].eep_id);
        if (slave_ == 0 &&
            std::strncmp(ec_slave[i].name, cfg.ethercat.slave_name.c_str(),
                         cfg.ethercat.slave_name.size()) == 0)
            slave_ = static_cast<uint16_t>(i);
    }
    if (slave_ == 0) {
        err = "Slave '" + cfg.ethercat.slave_name + "' nicht gefunden";
        return false;
    }
    // Revision from the SII; for one not contained in the device description, use the latest
    const auto& desc = DeviceDescription::el3356_0010();
    const uint32_t rev = ec_slave[slave_].eep_rev;
    const auto& revs = desc.revisions();
    revision_ = std::find(revs.begin(), revs.end(), rev) != revs.end() ? rev : 0U;
    if (revision_ == 0U)
        log_warn("Revision 0x%08X nicht in der Gerätebeschreibung (bis 0x%08X); verwende deren neueste", rev,
                 desc.newest_revision());
    g_revision = revision_;
    ec_slave[slave_].PO2SOconfig = &po2so_hook;

    const int used = ec_config_map(iomap_.data());
    if (used <= 0 || used > static_cast<int>(iomap_.size())) {
        err = "ec_config_map fehlgeschlagen";
        return false;
    }
    // ec_statecheck(0, ...) only updates ec_slave[0].state (group state via BRD);
    // the individual states are only provided by ec_readstate().
    ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * kSafeOpTimeoutFactor);
    ec_readstate();
    if (ec_slave[slave_].state != EC_STATE_SAFE_OP) {
        err = "SAFE-OP nicht erreicht: " + slave_states(EC_STATE_SAFE_OP);
        return false;
    }
    if (!read_layout(err)) return false;

    expected_wkc_ = ec_group[0].outputsWKC * kOutputWkcFactor + ec_group[0].inputsWKC;
    log_info("IOmap %d Byte, erwarteter WKC %d", used, expected_wkc_);

    ec_slave[0].state = EC_STATE_OPERATIONAL;
    ec_send_processdata();
    ec_receive_processdata(EC_TIMEOUTRET);
    ec_writestate(0);
    for (int i = 0; i < kOpRetries && ec_slave[0].state != EC_STATE_OPERATIONAL; ++i) {
        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);
        ec_statecheck(0, EC_STATE_OPERATIONAL, kOpCheckTimeoutUs);
    }
    if (ec_slave[0].state != EC_STATE_OPERATIONAL) {
        ec_readstate();
        err = "OP nicht erreicht: " + slave_states(EC_STATE_OPERATIONAL);
        return false;
    }
    link_ok_ = true;
    log_info("EtherCAT in OP, Slave %u", slave_);
    return true;
}

/**
 * @brief Reads the PDO assignment and computes the position of the inputs and outputs.
 * @param[out] err error text
 * @return false if the mapping cannot be read or Value (Real) is missing
 */
bool EthercatBackend::read_layout(std::string& err) {
    std::vector<std::vector<el3356::MapEntry>> in, out;
    std::vector<uint16_t> in_idx, out_idx;
    if (!read_assigned(slave_, el3356::kSm3Assign, in, in_idx) ||
        !read_assigned(slave_, el3356::kSm2Assign, out, out_idx)) {
        err = "PDO-Mapping konnte nicht gelesen werden";
        return false;
    }
    discover_pdos(out_idx, in_idx);
    in_ = el3356::layout_inputs(in);
    out_ = el3356::layout_outputs(out);
    const uint32_t ib = ec_slave[slave_].Istartbit, ob = ec_slave[slave_].Ostartbit;
    for (auto* l : {&in_.overrange, &in_.data_invalid, &in_.error, &in_.calibration,
                    &in_.steady_state, &in_.sync_error, &in_.toggle, &in_.value_int, &in_.value_real})
        shift(*l, ib);
    for (auto* l : {&out_.start_calibration, &out_.disable_calibration, &out_.input_freeze,
                    &out_.sample_mode, &out_.tare, &out_.filter_frequency})
        shift(*l, ob);
    if (!in_.value_real.present && !in_.value_int.present) {
        err = "Weder 0x6000:12 Value (Real) noch 0x6000:11 Value (INT32) ist gemappt";
        return false;
    }
    return true;
}

/**
 * @brief Reads all PDO mapping objects 0x1600..0x1607 and 0x1A00..0x1A07 (only during startup).
 *
 * Names and types via SDO information, otherwise from the ESI; exclude lists from the ESI.
 *
 * @param rx_assign content of 0x1C12
 * @param tx_assign content of 0x1C13
 */
void EthercatBackend::discover_pdos(const std::vector<uint16_t>& rx_assign,
                                    const std::vector<uint16_t>& tx_assign) {
    auto info = std::make_unique<SdoInfo>();
    info->slave = slave_;
    info->supported = (ec_slave[slave_].CoEdetails & ECT_COEDET_SDOINFO) != 0;
    const auto catalog = el3356::known_pdos();
    pdos_.clear();
    for (uint16_t base : {el3356::kPdoRxFirst, el3356::kPdoTxFirst})
        for (uint16_t idx = base; idx < base + el3356::kPdoRangeSize; ++idx) {
            std::vector<el3356::MapEntry> m;
            if (!read_pdo_mapping(slave_, idx, m)) continue;
            el3356::PdoInfo p;
            p.index = idx;
            p.rx = base == el3356::kPdoRxFirst;
            if (!info->object_name(idx, p.name)) p.name = el3356::known_pdo_name(idx);
            for (const auto& k : catalog)
                if (k.index == idx) p.excludes = k.excludes;
            for (const auto& me : m) {
                el3356::PdoEntry e;
                e.map = me;
                if (me.index != 0) {
                    uint16_t coe = 0;
                    std::string known_name;
                    el3356::DType known_type = el3356::DType::None;
                    const bool has_known = el3356::known_entry(me.index, me.sub, known_name, known_type);
                    if (!info->entry(me.index, me.sub, e.name, coe)) e.name = known_name;
                    e.type = coe ? el3356::dtype_from_coe(coe, me.bits)
                                 : has_known ? known_type : el3356::dtype_from_coe(el3356::kCoeUnknown, me.bits);
                    if (e.name.empty()) {
                        char b[kObjNameLen];
                        std::snprintf(b, sizeof b, "0x%04X:%02X", me.index, me.sub);
                        e.name = b;
                    }
                }
                p.entries.push_back(std::move(e));
            }
            pdos_.push_back(std::move(p));
        }
    el3356::locate_pdos(pdos_, rx_assign, tx_assign);
    for (auto& p : pdos_)
        for (auto& e : p.entries)
            shift(e.loc, p.rx ? ec_slave[slave_].Ostartbit : ec_slave[slave_].Istartbit);
    // Do not leave SDO aborts of non-existent objects standing as errors
    while (ec_iserror()) {
        ec_errort e;
        if (!ec_poperror(&e)) break;
    }
    log_info("%zu PDOs der Klemme eingelesen", pdos_.size());
}

/// @copydoc IoBackend::process_image
void EthercatBackend::process_image(std::vector<uint8_t>& in, std::vector<uint8_t>& out) const {
    in.clear();
    out.clear();
    if (!opened_ || slave_ == 0) return;
    const ec_slavet& sl = ec_slave[slave_];
    // Ibytes/Obytes are 0 for fewer than 8 bits
    const size_t ib = sl.Ibytes ? sl.Ibytes : (sl.Ibits ? 1 : 0);
    const size_t ob = sl.Obytes ? sl.Obytes : (sl.Obits ? 1 : 0);
    if (sl.inputs) in.assign(sl.inputs, sl.inputs + ib);
    if (sl.outputs) out.assign(sl.outputs, sl.outputs + ob);
}

/**
 * @brief Process data cycle; after wkc_fault_cycles WKC errors, check_and_recover() follows.
 * @param t time since start [s]
 * @param ctrl control bits
 * @param[out] out sample
 */
void EthercatBackend::cycle(double t, const El3356Control& ctrl, Sample& out) {
    out = {};
    out.t = t;
    if (!opened_ || slave_ == 0) return;

    el3356::encode_control(ec_slave[slave_].outputs, out_, ctrl);
    ec_send_processdata();
    const int wkc = ec_receive_processdata(EC_TIMEOUTRET);

    out.wkc_ok = wkc >= expected_wkc_;
    if (out.wkc_ok) {
        wkc_fail_count_ = 0;
        link_ok_ = true;
    } else if (++wkc_fail_count_ >= wkc_fault_cycles_) {
        check_and_recover(t);
    }
    out.link_ok = link_ok_;
    out.status = el3356::decode_status(ec_slave[slave_].inputs, in_);
    out.raw = el3356::decode_measurement(ec_slave[slave_].inputs, in_);
}

/**
 * @brief State check and restart (based on SOEM simple_test/ecatcheck).
 *
 * At most every 0.5 s: acknowledge errors, SAFE-OP -> OP, reconfigure slaves
 * or recover them.
 *
 * @param t time since start [s]
 */
void EthercatBackend::check_and_recover(double t) {
    if (last_check_t_ != kNoCheck && t - last_check_t_ < kCheckIntervalS) return;
    last_check_t_ = t;
    ec_readstate();
    link_ok_ = true;
    assert(ec_slavecount < UINT16_MAX);
    for (uint16_t s = 1; s <= static_cast<uint16_t>(ec_slavecount); ++s) {
        ec_slavet& sl = ec_slave[s];
        if (sl.state == EC_STATE_OPERATIONAL) continue;
        link_ok_ = false;
        if (sl.state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
            log_warn("Slave %u: SAFE-OP+ERROR, quittiere", s);
            sl.state = EC_STATE_SAFE_OP + EC_STATE_ACK;
            ec_writestate(s);
        } else if (sl.state == EC_STATE_SAFE_OP) {
            log_warn("Slave %u: SAFE-OP -> OP", s);
            sl.state = EC_STATE_OPERATIONAL;
            ec_writestate(s);
        } else if (sl.state > EC_STATE_NONE) {
            if (ec_reconfig_slave(s, kRecoverTimeoutUs)) {
                sl.islost = FALSE;
                log_warn("Slave %u: rekonfiguriert", s);
            }
        } else if (!sl.islost) {
            ec_statecheck(s, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
            if (sl.state == EC_STATE_NONE) {
                sl.islost = TRUE;
                log_error("Slave %u: verloren", s);
            }
        }
        if (sl.islost && sl.state == EC_STATE_NONE) {
            if (ec_recover_slave(s, kRecoverTimeoutUs)) {
                sl.islost = FALSE;
                log_warn("Slave %u: wiederhergestellt", s);
            }
        }
    }
}

/// @copydoc IoBackend::write_device_config
bool EthercatBackend::write_device_config(const DeviceConfig& dc, std::string& err) {
    if (!opened_ || slave_ == 0) {
        err = "EtherCAT nicht initialisiert";
        return false;
    }
    if (!write_settings(slave_, dc, revision_, err)) return false;
    cfg_.device = dc;
    return true;
}

/// @copydoc IoBackend::sdo_write
bool EthercatBackend::sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                                std::string& err) {
    if (!opened_ || slave_ == 0) {
        err = "EtherCAT nicht initialisiert";
        return false;
    }
    auto buf = data;
    if (ec_SDOwrite(slave_, index, sub, FALSE, static_cast<int>(buf.size()), buf.data(),
                    EC_TIMEOUTRXM) <= 0) {
        char b[kMsgLen];
        std::snprintf(b, sizeof b, "SDO-Write 0x%04X:%02X fehlgeschlagen", index, sub);
        err = b;
        return false;
    }
    return true;
}

/// @copydoc IoBackend::sdo_read
bool EthercatBackend::sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                               std::string& err) {
    if (!opened_ || slave_ == 0) {
        err = "EtherCAT nicht initialisiert";
        return false;
    }
    data.assign(size, 0);
    int n = static_cast<int>(size);
    if (ec_SDOread(slave_, index, sub, FALSE, &n, data.data(), EC_TIMEOUTRXM) <= 0) {
        char b[kMsgLen];
        std::snprintf(b, sizeof b, "SDO-Read 0x%04X:%02X fehlgeschlagen", index, sub);
        err = b;
        return false;
    }
    data.resize(static_cast<size_t>(n));
    return true;
}

/// @brief All slaves to INIT, close SOEM, discard the PDO catalog.
void EthercatBackend::close() {
    if (!opened_) return;
    ec_slave[0].state = EC_STATE_INIT;
    ec_writestate(0);
    ec_close();
    opened_ = false;
    pdos_.clear();
    g_cfg = nullptr;
}

} // namespace ew
