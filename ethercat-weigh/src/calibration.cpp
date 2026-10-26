#include "calibration.hpp"

#include <algorithm>

#include "units.hpp"

namespace ew {

/**
 * @brief Creates the characteristic from the configuration.
 * @param cal calibration method, unit of the terminal and calibration points
 * @param dev terminal parameters (scale factor)
 * @return characteristic; valid() == false for an invalid unit, scale factor 0 or
 *         invalid calibration points
 */
Characteristic Characteristic::from_config(const CalibrationConfig& cal, const DeviceConfig& dev) {
    Characteristic c;
    c.mode_ = cal.mode;
    if (cal.mode == "application") {
        c.pts_ = cal.points;
        std::sort(c.pts_.begin(), c.pts_.end(),
                  [](const CalPoint& a, const CalPoint& b) { return a.raw < b.raw; });
        c.valid_ = validate_points(c.pts_).empty();
    } else {
        const auto gpu = grams_per_unit(cal.device_unit);
        c.valid_ = gpu.has_value() && dev.scale_factor != 0.0;
        if (c.valid_) c.factor_ = *gpu / dev.scale_factor;
    }
    return c;
}

/**
 * @brief Converts a raw value to grams.
 * @param raw Value (Real) of the terminal
 * @return mass in grams, std::nullopt for an invalid characteristic
 */
std::optional<double> Characteristic::to_grams(double raw) const {
    if (!valid_) return std::nullopt;
    if (mode_ != "application") return raw * factor_;

    // Select segment: first segment for raw < p0, last for raw > pn
    size_t i = 1;
    while (i < pts_.size() - 1 && raw > pts_[i].raw) ++i;
    const CalPoint& a = pts_[i - 1];
    const CalPoint& b = pts_[i];
    return a.mass_g + (raw - a.raw) * (b.mass_g - a.mass_g) / (b.raw - a.raw);
}

/**
 * @brief Slope in grams per raw value unit.
 * @return device: constant factor; application: mean slope over all points;
 *         0 for an invalid characteristic
 */
double Characteristic::sensitivity() const {
    if (!valid_) return 0.0;
    if (mode_ != "application") return factor_;
    const auto& a = pts_.front();
    const auto& b = pts_.back();
    return (b.mass_g - a.mass_g) / (b.raw - a.raw);
}

} // namespace ew
