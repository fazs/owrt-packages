#pragma once

#include <string>

#include "el3356.hpp"

namespace ew {

/// One process data cycle: raw value of the terminal and the masses derived from it.
struct Sample {
    double t = 0.0;        ///< seconds since start (monotonic)
    double raw = 0.0;      ///< value of the terminal (REAL or INT32), unchanged
    double gross = 0.0;    ///< gross [g] after the characteristic
    double value = 0.0;    ///< net [g] = gross - tare (software)
    El3356Status status;   ///< status bits of the terminal
    bool wkc_ok = false;      ///< working counter as expected
    bool link_ok = false;     ///< Slave in OP
    bool calibrated = true;   ///< characteristic valid
    bool calibrating = false; ///< calibration in progress
};

/**
 * @brief Checks whether a sample is valid.
 * @param s Sample
 * @param[out] reason reason for failure ("link", "wkc", "error", "overrange", "data_invalid",
 *             "calibration", "calibrating", "uncalibrated"), empty for a valid sample;
 *             may be nullptr
 * @return true if the sample may be used for the measurement
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
