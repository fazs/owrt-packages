#include "measurement_csv.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "units.hpp"

namespace ew {

namespace {
constexpr int kCsvPrecision = 9;          ///< signifikante Stellen im Export
constexpr char kAltSeparator = ',';       ///< Trennzeichen fremder CSV-Dateien (dann Dezimalpunkt)
constexpr const char* kUtf8Bom = "\xEF\xBB\xBF";  ///< Byte Order Mark am Dateianfang

/**
 * @brief Teilt eine Zeile am Trennzeichen.
 * @param line Zeile ohne Zeilenende
 * @param sep Trennzeichen
 * @return Felder ohne umgebende Leerzeichen
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
 * @brief Liest eine Zahl; bei Semikolon als Trennzeichen auch mit Dezimalkomma.
 * @param s Feld
 * @param sep Trennzeichen der Datei
 * @return Wert, std::nullopt bei leerem oder ungültigem Feld
 */
std::optional<double> parse_num(std::string s, char sep) {
    if (sep == kCsvSeparator) std::replace(s.begin(), s.end(), ',', '.');
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return std::nullopt;
    return v;
}

/// Spaltenpositionen einer Datei; std::nullopt = Spalte fehlt.
struct Columns {
    std::optional<size_t> t;       ///< t_s
    std::optional<size_t> value;   ///< value_<u> bzw. einzige Spalte
    std::optional<size_t> stddev;  ///< stddev_<u>
    std::optional<size_t> range;   ///< range_<u>
    std::optional<size_t> settle;  ///< settle_ms
    std::string unit = "g";        ///< Einheit der Massenspalten
};

/**
 * @brief Ordnet die Spalten anhand der Kopfzeile zu.
 * @param head Felder der Kopfzeile
 * @return Spaltenpositionen
 * @throws std::runtime_error ohne Spalte value_<u> oder bei unbekannter Einheit
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
 * @brief Kopfzeile des CSV-Exports.
 * @param unit Einheit der Massenspalten
 * @return Zeile inkl. Zeilenende
 */
std::string measurements_csv_header(const std::string& unit) {
    std::ostringstream os;
    os << kCsvColId << kCsvSeparator << kCsvColTime << kCsvSeparator << kCsvColValue << unit
       << kCsvSeparator << kCsvColStddev << unit << kCsvSeparator << kCsvColRange << unit
       << kCsvSeparator << kCsvColSamples << kCsvSeparator << kCsvColSettle << '\n';
    return os.str();
}

/**
 * @brief Eine Zeile des CSV-Exports.
 * @param m Messung (Massen in Gramm)
 * @param unit Einheit der Massenspalten
 * @return Zeile inkl. Zeilenende
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
 * @brief Liest Messungen aus einer CSV-Datei.
 *
 * - Format des Exports (Kopfzeile, Trennzeichen ';', Dezimalpunkt oder -komma); Spalten werden
 *   über die Kopfzeile gefunden, nur value_<u> ist Pflicht, die Reihenfolge ist beliebig.
 * - Trennzeichen ',' (dann nur Dezimalpunkt).
 * - Ohne Kopfzeile: ein Wert in Gramm je Zeile.
 *
 * Leere Zeilen werden übersprungen; id und n werden nicht übernommen (n bleibt 0). Massen werden in Gramm umgerechnet.
 *
 * @param in Eingabe
 * @return Messungen in Dateireihenfolge
 * @throws std::runtime_error bei fehlender Wertspalte, unbekannter Einheit oder ungültiger Zahl
 *         (mit Zeilennummer)
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
                cols = Columns{};  // keine Kopfzeile: einzige Spalte = Wert in Gramm (auch "12,5")
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
