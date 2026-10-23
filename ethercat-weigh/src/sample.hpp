#pragma once

#include <cstdint>
#include <string>

namespace ew {

// Statusbits aus TxPDO 0x1A00 "RMB Status" (Objekt 0x6000)
struct El3356Status {
    bool overrange = false;               // 0x6000:02
    bool data_invalid = false;            // 0x6000:04
    bool error = false;                   // 0x6000:07
    bool calibration_in_progress = false; // 0x6000:08
    bool steady_state = false;            // 0x6000:09
    bool sync_error = false;              // 0x6000:0E
    bool txpdo_toggle = false;            // 0x6000:10
};

// Steuerbits aus RxPDO 0x1600 "RMB Control" (Objekt 0x7000)
struct El3356Control {
    bool start_calibration = false;   // 0x7000:01
    bool disable_calibration = false; // 0x7000:02
    bool input_freeze = false;        // 0x7000:03
    bool sample_mode = false;         // 0x7000:04
    bool tare = false;                // 0x7000:05
};

struct Sample {
    double t = 0.0;        // Sekunden seit Start (monoton)
    double raw = 0.0;      // Value (Real) der Klemme, unverändert
    double gross = 0.0;    // Brutto [g] nach Kennlinie
    double value = 0.0;    // Netto [g] = gross - Tara (Software)
    El3356Status status;
    bool wkc_ok = false;
    bool link_ok = false;     // Slave in OP
    bool calibrated = true;   // Kennlinie gültig
    bool calibrating = false; // Kalibriervorgang aktiv
};

// Prüft die Gültigkeit eines Samples. reason wird bei false gesetzt.
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
