#include "ethercat_backend.hpp"

#include <cstdio>
#include <cstring>

#include "ethercat.h"
#include "log.hpp"

namespace ew {

namespace {

// Kontext für den PRE-OP -> SAFE-OP Hook (SOEM ruft eine freie Funktion auf)
const Config* g_cfg = nullptr;

bool sdo_write(uint16_t slave, uint16_t idx, uint8_t sub, const void* data, int size) {
    std::vector<uint8_t> buf(static_cast<const uint8_t*>(data),
                             static_cast<const uint8_t*>(data) + size);
    return ec_SDOwrite(slave, idx, sub, FALSE, size, buf.data(), EC_TIMEOUTRXM) > 0;
}

template <typename T>
bool sdo_write_v(uint16_t slave, uint16_t idx, uint8_t sub, T v) {
    return sdo_write(slave, idx, sub, &v, sizeof(T));
}

template <typename T>
bool sdo_read_v(uint16_t slave, uint16_t idx, uint8_t sub, T& v) {
    int size = sizeof(T);
    v = T{};
    return ec_SDOread(slave, idx, sub, FALSE, &size, &v, EC_TIMEOUTRXM) > 0;
}

bool write_assign(uint16_t slave, uint16_t sm_obj, const std::vector<uint16_t>& pdos) {
    bool ok = sdo_write_v<uint8_t>(slave, sm_obj, 0, 0);
    for (size_t i = 0; i < pdos.size(); ++i)
        ok = ok && sdo_write_v<uint16_t>(slave, sm_obj, static_cast<uint8_t>(i + 1), pdos[i]);
    ok = ok && sdo_write_v<uint8_t>(slave, sm_obj, 0, static_cast<uint8_t>(pdos.size()));
    return ok;
}

bool write_settings(uint16_t slave, const DeviceConfig& dc, std::string& err) {
    for (const auto& w : el3356::settings_writes(dc)) {
        auto data = w.data;
        if (ec_SDOwrite(slave, w.index, w.sub, FALSE, static_cast<int>(data.size()), data.data(),
                        EC_TIMEOUTRXM) <= 0) {
            char b[96];
            std::snprintf(b, sizeof b, "SDO-Write 0x%04X:%02X (%s) fehlgeschlagen", w.index, w.sub,
                          w.name.c_str());
            err = b;
            return false;
        }
    }
    return true;
}

// PRE-OP -> SAFE-OP: PDO-Zuordnung und CoE-Parameter setzen
int po2so_hook(uint16 slave) {
    using namespace el3356;
    if (!write_assign(slave, kSm2Assign, {kPdoControl}))
        log_error("PDO-Zuordnung 0x1C12 (0x1600) konnte nicht geschrieben werden");
    if (!write_assign(slave, kSm3Assign, {kPdoStatus, kPdoValueReal}))
        log_error("PDO-Zuordnung 0x1C13 (0x1A00, 0x1A02) konnte nicht geschrieben werden");
    else
        log_info("PDO-Zuordnung gesetzt: 0x1C12 = 0x1600, 0x1C13 = 0x1A00, 0x1A02");
    if (g_cfg && g_cfg->device.write_on_start) {
        std::string err;
        if (!write_settings(slave, g_cfg->device, err)) log_error(err);
    }
    return 1;
}

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

bool read_assigned(uint16_t slave, uint16_t sm_obj, std::vector<std::vector<el3356::MapEntry>>& out) {
    uint8_t n = 0;
    if (!sdo_read_v(slave, sm_obj, 0, n)) return false;
    out.clear();
    for (uint8_t i = 1; i <= n; ++i) {
        uint16_t pdo = 0;
        if (!sdo_read_v(slave, sm_obj, i, pdo)) return false;
        std::vector<el3356::MapEntry> m;
        if (!read_pdo_mapping(slave, pdo, m)) return false;
        out.push_back(std::move(m));
    }
    return true;
}

const char* state_name(uint16_t st) {
    switch (st & 0x0F) {
    case EC_STATE_INIT: return "INIT";
    case EC_STATE_PRE_OP: return "PRE-OP";
    case EC_STATE_BOOT: return "BOOT";
    case EC_STATE_SAFE_OP: return "SAFE-OP";
    case EC_STATE_OPERATIONAL: return "OP";
    default: return "NONE";
    }
}

// Zustand, Fehlerbit und AL-Statuscode aller Slaves, die 'expected' nicht erreicht haben.
// Voraussetzung: ec_readstate() wurde unmittelbar zuvor aufgerufen.
std::string slave_states(uint16_t expected) {
    std::string out;
    for (int i = 1; i <= ec_slavecount; ++i) {
        const ec_slavet& sl = ec_slave[i];
        if (sl.state == expected) continue;
        char b[256];
        std::snprintf(b, sizeof b, "%sSlave %d %s: %s%s, AL-Status 0x%04X (%s)",
                      out.empty() ? "" : "; ", i, sl.name, state_name(sl.state),
                      (sl.state & EC_STATE_ERROR) ? "+ERROR" : "", sl.ALstatuscode,
                      ec_ALstatuscode2string(sl.ALstatuscode));
        out += b;
    }
    return out.empty() ? "Gruppenzustand abweichend" : out;
}

void shift(el3356::EntryLoc& l, uint32_t bits) {
    if (l.present) l.bit_offset += bits;
}

} // namespace

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
    ec_slave[slave_].PO2SOconfig = &po2so_hook;

    const int used = ec_config_map(iomap_.data());
    if (used <= 0 || used > static_cast<int>(iomap_.size())) {
        err = "ec_config_map fehlgeschlagen";
        return false;
    }
    // ec_statecheck(0, ...) aktualisiert nur ec_slave[0].state (Gruppenzustand per BRD);
    // die Einzelzustände liefert erst ec_readstate().
    ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
    ec_readstate();
    if (ec_slave[slave_].state != EC_STATE_SAFE_OP) {
        err = "SAFE-OP nicht erreicht: " + slave_states(EC_STATE_SAFE_OP);
        return false;
    }
    if (!read_layout(err)) return false;

    expected_wkc_ = ec_group[0].outputsWKC * 2 + ec_group[0].inputsWKC;
    log_info("IOmap %d Byte, erwarteter WKC %d", used, expected_wkc_);

    ec_slave[0].state = EC_STATE_OPERATIONAL;
    ec_send_processdata();
    ec_receive_processdata(EC_TIMEOUTRET);
    ec_writestate(0);
    for (int i = 0; i < 200 && ec_slave[0].state != EC_STATE_OPERATIONAL; ++i) {
        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);
        ec_statecheck(0, EC_STATE_OPERATIONAL, 50000);
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

bool EthercatBackend::read_layout(std::string& err) {
    std::vector<std::vector<el3356::MapEntry>> in, out;
    if (!read_assigned(slave_, el3356::kSm3Assign, in) ||
        !read_assigned(slave_, el3356::kSm2Assign, out)) {
        err = "PDO-Mapping konnte nicht gelesen werden";
        return false;
    }
    in_ = el3356::layout_inputs(in);
    out_ = el3356::layout_outputs(out);
    const uint32_t ib = ec_slave[slave_].Istartbit, ob = ec_slave[slave_].Ostartbit;
    for (auto* l : {&in_.overrange, &in_.data_invalid, &in_.error, &in_.calibration,
                    &in_.steady_state, &in_.sync_error, &in_.toggle, &in_.value_real})
        shift(*l, ib);
    for (auto* l : {&out_.start_calibration, &out_.disable_calibration, &out_.input_freeze,
                    &out_.sample_mode, &out_.tare})
        shift(*l, ob);
    if (!in_.value_real.present) {
        err = "0x6000:12 Value (Real) ist nicht gemappt";
        return false;
    }
    return true;
}

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
    out.raw = el3356::decode_value_real(ec_slave[slave_].inputs, in_);
}

// Zustandsprüfung und Wiederanlauf (angelehnt an SOEM simple_test/ecatcheck)
void EthercatBackend::check_and_recover(double t) {
    if (last_check_t_ >= 0.0 && t - last_check_t_ < 0.5) return;
    last_check_t_ = t;
    ec_readstate();
    link_ok_ = true;
    for (uint16_t s = 1; s <= ec_slavecount; ++s) {
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
            if (ec_reconfig_slave(s, 500)) {
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
            if (ec_recover_slave(s, 500)) {
                sl.islost = FALSE;
                log_warn("Slave %u: wiederhergestellt", s);
            }
        }
    }
}

bool EthercatBackend::write_device_config(const DeviceConfig& dc, std::string& err) {
    if (!opened_ || slave_ == 0) {
        err = "EtherCAT nicht initialisiert";
        return false;
    }
    if (!write_settings(slave_, dc, err)) return false;
    cfg_.device = dc;
    return true;
}

bool EthercatBackend::sdo_write(uint16_t index, uint8_t sub, const std::vector<uint8_t>& data,
                                std::string& err) {
    if (!opened_ || slave_ == 0) {
        err = "EtherCAT nicht initialisiert";
        return false;
    }
    auto buf = data;
    if (ec_SDOwrite(slave_, index, sub, FALSE, static_cast<int>(buf.size()), buf.data(),
                    EC_TIMEOUTRXM) <= 0) {
        char b[64];
        std::snprintf(b, sizeof b, "SDO-Write 0x%04X:%02X fehlgeschlagen", index, sub);
        err = b;
        return false;
    }
    return true;
}

bool EthercatBackend::sdo_read(uint16_t index, uint8_t sub, size_t size, std::vector<uint8_t>& data,
                               std::string& err) {
    if (!opened_ || slave_ == 0) {
        err = "EtherCAT nicht initialisiert";
        return false;
    }
    data.assign(size, 0);
    int n = static_cast<int>(size);
    if (ec_SDOread(slave_, index, sub, FALSE, &n, data.data(), EC_TIMEOUTRXM) <= 0) {
        char b[64];
        std::snprintf(b, sizeof b, "SDO-Read 0x%04X:%02X fehlgeschlagen", index, sub);
        err = b;
        return false;
    }
    data.resize(static_cast<size_t>(n));
    return true;
}

void EthercatBackend::close() {
    if (!opened_) return;
    ec_slave[0].state = EC_STATE_INIT;
    ec_writestate(0);
    ec_close();
    opened_ = false;
    g_cfg = nullptr;
}

} // namespace ew
