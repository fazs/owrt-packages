#include "measurement_csv.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "units.hpp"

namespace ew {

namespace {
constexpr int kCsvPrecision = 9;          ///< significant digits in the export
constexpr char kAltSeparator = ',';       ///< separator of foreign CSV files (then decimal point)
constexpr const char* kUtf8Bom = "\xEF\xBB\xBF";  ///< byte order mark at the start of the file

/**
 * @brief Splits a line at the separator.
 * @param line line without line ending
 * @param sep separator
 * @return fields without surrounding spaces
 */
std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream is(line);
    while (std::getline(is, field, sep)) {
        const auto b = field.find_first_not_of(" \t");
        const auto e = field.find_last_not_of(" \t");
        out.push_back(b == std::string::npos ? std::string() : field.substr(b, e - b + 1));
    }
    if (!line.empty() && line.back() == sep) out.emplace_back();
    return out;
}

/**
 * @brief Reads a number; with semicolon as separator also with decimal comma.
 * @param s field
 * @param sep separator of the file
 * @return value, std::nullopt for an empty or invalid field
 */
std::optional<double> parse_num(std::string s, char sep) {
    if (sep == kCsvSeparator) std::replace(s.begin(), s.end(), ',', '.');
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return std::nullopt;
    return v;
}

/// Column positions of a file; std::nullopt = column missing.
struct Columns {
    std::optional<size_t> t;       ///< t_s
    std::optional<size_t> value;   ///< value_<u> or the only column
    std::optional<size_t> stddev;  ///< stddev_<u>
    std::optional<size_t> range;   ///< range_<u>
    std::optional<size_t> settle;  ///< settle_ms
    std::string unit = "g";        ///< unit of the mass columns
};

/**
 * @brief Assigns the columns based on the header line.
 * @param head fields of the header line
 * @return column positions
 * @throws std::runtime_error without column value_<u> or for an unknown unit
 */
Columns columns_from_header(const std::vector<std::string>& head) {
    Columns c;
    const std::string value_prefix = kCsvColValue;
    for (size_t i = 0; i < head.size(); ++i) {
        const std::string& h = head[i];
        if (h == kCsvColTime) c.t = i;
        else if (h == kCsvColSettle) c.settle = i;
        else if (h.rfind(value_prefix, 0) == 0) {
            c.value = i;
            c.unit = h.substr(value_prefix.size());
        }
    }
    if (!c.value) throw std::runtime_error("CSV: Spalte value_<einheit> fehlt");
    if (!valid_unit(c.unit)) throw std::runtime_error("CSV: unbekannte Einheit '" + c.unit + "'");
    const auto find = [&](const std::string& name) -> std::optional<size_t> {
        const auto it = std::find(head.begin(), head.end(), name);
        if (it == head.end()) return std::nullopt;
        return static_cast<size_t>(it - head.begin());
    };
    c.stddev = find(kCsvColStddev + c.unit);
    c.range = find(kCsvColRange + c.unit);
    return c;
}
} // namespace

/**
 * @brief Header line of the CSV export.
 * @param unit unit of the mass columns
 * @return line incl. line ending
 */
std::string measurements_csv_header(const std::string& unit) {
    std::ostringstream os;
    os << kCsvColId << kCsvSeparator << kCsvColTime << kCsvSeparator << kCsvColValue << unit
       << kCsvSeparator << kCsvColStddev << unit << kCsvSeparator << kCsvColRange << unit
       << kCsvSeparator << kCsvColSamples << kCsvSeparator << kCsvColSettle << '\n';
    return os.str();
}

/**
 * @brief One line of the CSV export.
 * @param m measurement (masses in grams)
 * @param unit unit of the mass columns
 * @return line incl. line ending
 */
std::string measurement_csv_row(const Measurement& m, const std::string& unit) {
    std::ostringstream os;
    os.precision(kCsvPrecision);
    os << m.id << kCsvSeparator << m.t << kCsvSeparator << from_grams(m.value, unit)
       << kCsvSeparator << from_grams(m.stddev, unit) << kCsvSeparator
       << from_grams(m.range, unit) << kCsvSeparator << m.n << kCsvSeparator << m.settle_ms
       << '\n';
    return os.str();
}

/**
 * @brief Reads measurements from a CSV file.
 *
 * - Export format (header line, separator ';', decimal point or comma); columns are
 *   found via the header line, only value_<u> is mandatory, the order is arbitrary.
 * - Separator ',' (then decimal point only).
 * - Without header line: one value in grams per line.
 *
 * Empty lines are skipped; id and n are not taken over (n stays 0). Masses are converted to grams.
 *
 * @param in input
 * @return measurements in file order
 * @throws std::runtime_error for a missing value column, unknown unit or invalid number
 *         (with line number)
 */
std::vector<Measurement> parse_measurements_csv(std::istream& in) {
    std::vector<Measurement> out;
    std::optional<Columns> cols;
    char sep = kCsvSeparator;
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line_no == 1 && line.rfind(kUtf8Bom, 0) == 0) line.erase(0, std::char_traits<char>::length(kUtf8Bom));
        if (line.find_first_not_of(" \t") == std::string::npos) continue;

        if (!cols) {
            const auto first = split(line, kCsvSeparator);
            if (first.size() == 1 && parse_num(first.front(), kCsvSeparator)) {
                cols = Columns{};  // no header line: only column = value in grams (also "12,5")
                cols->value = 0U;
            } else {
                if (line.find(kCsvSeparator) == std::string::npos && line.find(kAltSeparator) != std::string::npos)
                    sep = kAltSeparator;
                cols = columns_from_header(split(line, sep));
                continue;
            }
        }

        const auto f = split(line, sep);
        const auto num = [&](const std::optional<size_t>& col, bool required) -> double {
            if (!col) return 0.0;
            const auto v = *col < f.size() ? parse_num(f[*col], sep) : std::nullopt;
            if (!v && (required || (*col < f.size() && !f[*col].empty())))
                throw std::runtime_error("CSV Zeile " + std::to_string(line_no) + ": ungültige Zahl");
            return v.value_or(0.0);
        };
        Measurement m;
        m.value = to_grams(num(cols->value, true), cols->unit);
        m.t = num(cols->t, false);
        m.stddev = to_grams(num(cols->stddev, false), cols->unit);
        m.range = to_grams(num(cols->range, false), cols->unit);
        m.settle_ms = num(cols->settle, false);
        out.push_back(m);
    }
    return out;
}

} // namespace ew
