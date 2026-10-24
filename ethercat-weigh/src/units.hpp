#pragma once

#include <optional>
#include <string>

namespace ew {

/**
 * @file
 * @brief Masseneinheiten mg, g, kg, gr (interne Basiseinheit: Gramm) und Zeiteinheiten.
 */

/// @name Zeiteinheiten
/// @{
constexpr double kMsPerS = 1000.0;  ///< Millisekunden je Sekunde
constexpr double kUsPerMs = 1000.0; ///< Mikrosekunden je Millisekunde
/// @}

/// @name Gramm je Einheit
/// @{
constexpr double kGramsPerMg = 0.001;       ///< Milligramm
constexpr double kGramsPerG = 1.0;          ///< Gramm (Basiseinheit)
constexpr double kGramsPerKg = 1000.0;      ///< Kilogramm
constexpr double kGramsPerGr = 0.06479891;  ///< Grain: 1 gr = 64,79891 mg (exakte Definition)
/// @}

/**
 * @brief Umrechnungsfaktor einer Einheit in Gramm.
 * @param unit Einheit ("mg", "g", "kg", "gr")
 * @return Gramm je Einheit, std::nullopt bei unbekannter Einheit
 */
inline std::optional<double> grams_per_unit(const std::string& unit) {
    if (unit == "mg") return kGramsPerMg;
    if (unit == "g") return kGramsPerG;
    if (unit == "kg") return kGramsPerKg;
    if (unit == "gr") return kGramsPerGr;
    return std::nullopt;
}

/**
 * @brief Prüft, ob @p unit eine bekannte Masseneinheit ist.
 * @param unit Einheit
 * @return true für "mg", "g", "kg", "gr"
 */
inline bool valid_unit(const std::string& unit) { return grams_per_unit(unit).has_value(); }

/**
 * @brief Rechnet einen Wert in Gramm um.
 * @param value Wert in @p unit
 * @param unit Einheit; unbekannte Einheiten werden wie Gramm behandelt
 * @return Wert in Gramm
 */
inline double to_grams(double value, const std::string& unit) {
    return value * grams_per_unit(unit).value_or(kGramsPerG);
}

/**
 * @brief Rechnet Gramm in eine Einheit um.
 * @param grams Wert in Gramm
 * @param unit Zieleinheit; unbekannte Einheiten werden wie Gramm behandelt
 * @return Wert in @p unit
 */
inline double from_grams(double grams, const std::string& unit) {
    return grams / grams_per_unit(unit).value_or(kGramsPerG);
}

} // namespace ew
