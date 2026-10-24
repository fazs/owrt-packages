#pragma once

#include <cstdint>
#include <string>

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
};

/// Ein Prozessdatenzyklus: Rohwert der Klemme und daraus abgeleitete Massen.
struct Sample {
    double t = 0.0;        ///< Sekunden seit Start (monoton)
    double raw = 0.0;      ///< Value (Real) der Klemme, unverändert
    double gross = 0.0;    ///< Brutto [g] nach Kennlinie
    double value = 0.0;    ///< Netto [g] = gross - Tara (Software)
    El3356Status status;   ///< Statusbits der Klemme
    bool wkc_ok = false;      ///< Working Counter wie erwartet
    bool link_ok = false;     ///< Slave in OP
    bool calibrated = true;   ///< Kennlinie gültig
    bool calibrating = false; ///< Kalibriervorgang aktiv
};

/**
 * @brief Prüft die Gültigkeit eines Samples.
 * @param s Sample
 * @param[out] reason Fehlergrund ("link", "wkc", "error", "overrange", "data_invalid",
 *             "calibration", "calibrating", "uncalibrated"), leer bei gültigem Sample;
 *             darf nullptr sein
 * @return true, wenn das Sample für die Messung verwendet werden darf
 */
inline bool sample_valid(const Sample& s, std::string* reason = nullptr) {
    auto fail = [&](const char* r) {
        if (reason) *reason = r;
        return false;
    };
    if (!s.link_ok) return fail("link");
    if (!s.wkc_ok) return fail("wkc");
    if (s.status.error) return fail("error");
    if (s.status.overrange) return fail("overrange");
    if (s.status.data_invalid) return fail("data_invalid");
    if (s.status.calibration_in_progress) return fail("calibration");
    if (s.calibrating) return fail("calibrating");
    if (!s.calibrated) return fail("uncalibrated");
    if (reason) reason->clear();
    return true;
}

} // namespace ew
