#include "calibration.hpp"

#include <algorithm>

#include "units.hpp"

namespace ew {

/**
 * @brief Erzeugt die Kennlinie aus der Konfiguration.
 * @param cal Kalibrierverfahren, Einheit der Klemme und Stützpunkte
 * @param dev Klemmenparameter (Scale factor)
 * @return Kennlinie; valid() == false bei ungültiger Einheit, Scale factor 0 oder
 *         ungültigen Stützpunkten
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
 * @brief Rechnet einen Rohwert in Gramm um.
 * @param raw Value (Real) der Klemme
 * @return Masse in Gramm, std::nullopt bei ungültiger Kennlinie
 */
std::optional<double> Characteristic::to_grams(double raw) const {
    if (!valid_) return std::nullopt;
    if (mode_ != "application") return raw * factor_;

    // Segment wählen: erstes Segment für raw < p0, letztes für raw > pn
    size_t i = 1;
    while (i < pts_.size() - 1 && raw > pts_[i].raw) ++i;
    const CalPoint& a = pts_[i - 1];
    const CalPoint& b = pts_[i];
    return a.mass_g + (raw - a.raw) * (b.mass_g - a.mass_g) / (b.raw - a.raw);
}

/**
 * @brief Steigung in Gramm je Rohwert-Einheit.
 * @return device: konstanter Faktor; application: mittlere Steigung über alle Punkte;
 *         0 bei ungültiger Kennlinie
 */
double Characteristic::sensitivity() const {
    if (!valid_) return 0.0;
    if (mode_ != "application") return factor_;
    const auto& a = pts_.front();
    const auto& b = pts_.back();
    return (b.mass_g - a.mass_g) / (b.raw - a.raw);
}

} // namespace ew
