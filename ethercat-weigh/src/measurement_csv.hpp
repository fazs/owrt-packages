#pragma once

#include <istream>
#include <string>
#include <vector>

#include "measurement.hpp"

namespace ew {

/**
 * @file
 * @brief CSV format of the measurements: export (/api/measurements.csv) and import (--sim-csv).
 *
 * Header line: id;t_s;value_<u>;stddev_<u>;range_<u>;n;settle_ms (u = mg, g, kg, gr).
 */

/// @name Columns and separator
/// @{
constexpr char kCsvSeparator = ';';                    ///< separator of the export
constexpr const char* kCsvColId = "id";                ///< sequential number
constexpr const char* kCsvColTime = "t_s";             ///< time [s]
constexpr const char* kCsvColValue = "value_";         ///< value, prefix before the unit
constexpr const char* kCsvColStddev = "stddev_";       ///< standard deviation, prefix before the unit
constexpr const char* kCsvColRange = "range_";         ///< range, prefix before the unit
constexpr const char* kCsvColSamples = "n";            ///< samples in the window (export only)
constexpr const char* kCsvColSettle = "settle_ms";     ///< settling time [ms]
/// @}

std::string measurements_csv_header(const std::string& unit);

std::string measurement_csv_row(const Measurement& m, const std::string& unit);

std::vector<Measurement> parse_measurements_csv(std::istream& in);

} // namespace ew
