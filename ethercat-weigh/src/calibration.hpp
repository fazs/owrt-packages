#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config.hpp"

namespace ew {

/**
 * @brief Characteristic: raw value (0x6000:12 Value (Real)) -> mass in grams.
 *
 * - mode "device": g = raw / scale_factor * g(device_unit)
 *   (the characteristic is in the terminal; Value = load[device_unit] * scale factor)
 * - mode "application": piecewise linear interpolation over the calibration points,
 *   outside extrapolation with the first or last segment
 */
class Characteristic {
public:
    static Characteristic from_config(const CalibrationConfig& cal, const DeviceConfig& dev);

    [[nodiscard]] std::optional<double> to_grams(double raw) const;
    /// true if the characteristic is usable
    [[nodiscard]] bool valid() const { return valid_; }
    /// "device" or "application"
    [[nodiscard]] const std::string& mode() const { return mode_; }
    [[nodiscard]] double sensitivity() const;

private:
    std::string mode_;                    ///< "device" or "application"
    bool valid_ = false;                  ///< characteristic usable
    double factor_ = 1.0;                 ///< device: grams per raw value unit
    std::vector<CalPoint> pts_;           ///< application: calibration points, sorted by raw
};

} // namespace ew
