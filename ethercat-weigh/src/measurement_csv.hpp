#pragma once

#include <istream>
#include <string>
#include <vector>

#include "measurement.hpp"

namespace ew {

/**
 * @file
 * @brief CSV-Format der Messungen: Export (/api/measurements.csv) und Import (--sim-csv).
 *
 * Kopfzeile: id;t_s;value_<u>;stddev_<u>;range_<u>;n;settle_ms (u = mg, g, kg, gr).
 */

/// @name Spalten und Trennzeichen
/// @{
constexpr char kCsvSeparator = ';';                    ///< Trennzeichen des Exports
constexpr const char* kCsvColId = "id";                ///< fortlaufende Nummer
constexpr const char* kCsvColTime = "t_s";             ///< Zeitpunkt [s]
constexpr const char* kCsvColValue = "value_";         ///< Wert, Präfix vor der Einheit
constexpr const char* kCsvColStddev = "stddev_";       ///< Standardabweichung, Präfix vor der Einheit
constexpr const char* kCsvColRange = "range_";         ///< Spannweite, Präfix vor der Einheit
constexpr const char* kCsvColSamples = "n";            ///< Samples im Fenster (nur Export)
constexpr const char* kCsvColSettle = "settle_ms";     ///< Einschwingzeit [ms]
/// @}

std::string measurements_csv_header(const std::string& unit);

std::string measurement_csv_row(const Measurement& m, const std::string& unit);

std::vector<Measurement> parse_measurements_csv(std::istream& in);

} // namespace ew
