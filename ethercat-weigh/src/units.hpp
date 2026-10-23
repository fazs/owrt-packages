#pragma once

#include <optional>
#include <string>

namespace ew {

// Masseneinheiten. Interne Basiseinheit: Gramm.
// Grain: 1 gr = 64,79891 mg (exakte Definition).
inline std::optional<double> grams_per_unit(const std::string& unit) {
    if (unit == "mg") return 0.001;
    if (unit == "g") return 1.0;
    if (unit == "kg") return 1000.0;
    if (unit == "gr") return 0.06479891;
    return std::nullopt;
}

inline bool valid_unit(const std::string& unit) { return grams_per_unit(unit).has_value(); }

inline double to_grams(double value, const std::string& unit) {
    return value * grams_per_unit(unit).value_or(1.0);
}

inline double from_grams(double grams, const std::string& unit) {
    return grams / grams_per_unit(unit).value_or(1.0);
}

} // namespace ew
