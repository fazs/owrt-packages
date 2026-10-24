#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config.hpp"

namespace ew {

/**
 * @brief Kennlinie: Rohwert (0x6000:12 Value (Real)) -> Masse in Gramm.
 *
 * - mode "device": g = raw / scale_factor * g(device_unit)
 *   (Kennlinie liegt in der Klemme; Value = Last[device_unit] * Scale factor)
 * - mode "application": stückweise lineare Interpolation über die Stützpunkte,
 *   außerhalb Extrapolation mit dem ersten bzw. letzten Segment
 */
class Characteristic {
public:
    static Characteristic from_config(const CalibrationConfig& cal, const DeviceConfig& dev);

    [[nodiscard]] std::optional<double> to_grams(double raw) const;
    /// true, wenn die Kennlinie verwendbar ist
    [[nodiscard]] bool valid() const { return valid_; }
    /// "device" oder "application"
    [[nodiscard]] const std::string& mode() const { return mode_; }
    [[nodiscard]] double sensitivity() const;

private:
    std::string mode_;                    ///< "device" oder "application"
    bool valid_ = false;                  ///< Kennlinie verwendbar
    double factor_ = 1.0;                 ///< device: Gramm je Rohwert-Einheit
    std::vector<CalPoint> pts_;           ///< application: Stützpunkte, nach raw sortiert
};

} // namespace ew
