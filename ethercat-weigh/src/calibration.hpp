#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config.hpp"

namespace ew {

// Umrechnung Rohwert (0x6000:12 Value (Real)) -> Masse in Gramm.
//
//   mode "device":      g = raw / scale_factor * g(device_unit)
//                       (Kennlinie liegt in der Klemme; Value = Last[device_unit] * Scale factor)
//   mode "application": stückweise lineare Interpolation über die Stützpunkte,
//                       außerhalb Extrapolation mit dem ersten bzw. letzten Segment
class Characteristic {
public:
    static Characteristic from_config(const CalibrationConfig& cal, const DeviceConfig& dev);

    std::optional<double> to_grams(double raw) const;
    bool valid() const { return valid_; }
    const std::string& mode() const { return mode_; }
    // Steigung g pro Rohwert-Einheit (Anwendung: mittlere Steigung über alle Punkte)
    double sensitivity() const;

private:
    std::string mode_;
    bool valid_ = false;
    double factor_ = 1.0;                 // device
    std::vector<CalPoint> pts_;           // application, nach raw sortiert
};

} // namespace ew
