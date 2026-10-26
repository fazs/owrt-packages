#pragma once

#include <optional>
#include <string>

namespace ew {

/**
 * @file
 * @brief Mass units mg, g, kg, gr (internal base unit: gram) and time units.
 */

/// @name Time units
/// @{
constexpr double kMsPerS = 1000.0;  ///< milliseconds per second
constexpr double kUsPerMs = 1000.0; ///< microseconds per millisecond
/// @}

/// @name Grams per unit
/// @{
constexpr double kGramsPerMg = 0.001;       ///< milligram
constexpr double kGramsPerG = 1.0;          ///< gram (base unit)
constexpr double kGramsPerKg = 1000.0;      ///< kilogram
constexpr double kGramsPerGr = 0.06479891;  ///< grain: 1 gr = 64.79891 mg (exact definition)
/// @}

/**
 * @brief Conversion factor of a unit to grams.
 * @param unit unit ("mg", "g", "kg", "gr")
 * @return grams per unit, std::nullopt for an unknown unit
 */
inline std::optional<double> grams_per_unit(const std::string& unit) {
    if (unit == "mg") return kGramsPerMg;
    if (unit == "g") return kGramsPerG;
    if (unit == "kg") return kGramsPerKg;
    if (unit == "gr") return kGramsPerGr;
    return std::nullopt;
}

/**
 * @brief Checks whether @p unit is a known mass unit.
 * @param unit unit
 * @return true for "mg", "g", "kg", "gr"
 */
inline bool valid_unit(const std::string& unit) { return grams_per_unit(unit).has_value(); }

/**
 * @brief Converts a value to grams.
 * @param value value in @p unit
 * @param unit unit; unknown units are treated as grams
 * @return value in grams
 */
inline double to_grams(double value, const std::string& unit) {
    return value * grams_per_unit(unit).value_or(kGramsPerG);
}

/**
 * @brief Converts grams to a unit.
 * @param grams value in grams
 * @param unit target unit; unknown units are treated as grams
 * @return value in @p unit
 */
inline double from_grams(double grams, const std::string& unit) {
    return grams / grams_per_unit(unit).value_or(kGramsPerG);
}

} // namespace ew
