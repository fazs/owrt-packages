#pragma once

#include <string>

#include "el3356.hpp"

namespace ew {

/// Ein Prozessdatenzyklus: Rohwert der Klemme und daraus abgeleitete Massen.
struct Sample {
    double t = 0.0;        ///< Sekunden seit Start (monoton)
    double raw = 0.0;      ///< Value der Klemme (REAL bzw. INT32), unverändert
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
