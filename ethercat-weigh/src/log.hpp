#pragma once

#include <syslog.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace ew {

/**
 * @file
 * @brief Logausgabe.
 *
 * - Konsole (Default): Zeitstempel + Level nach stderr
 * - Syslog (--syslog): syslog(3), Facility LOG_DAEMON, Priorität je Level
 *   (INFO -> daemon.info, WARN -> daemon.warn, ERROR -> daemon.err)
 *
 * Unter procd wird --syslog verwendet; die Zuordnung stdout -> daemon.info,
 * stderr -> daemon.err von procd kennt keine Level.
 */

/// @brief Schalter "Ausgabe per syslog" (prozessweit).
inline std::atomic<bool>& log_syslog_flag() {
    static std::atomic<bool> f{false};
    return f;
}

/**
 * @brief Schaltet die Logausgabe auf syslog(3) um.
 * @param ident Kennung für openlog(3), z. B. "ethercat-weigh"
 */
inline void log_use_syslog(const char* ident) {
    openlog(ident, LOG_PID, LOG_DAEMON);
    log_syslog_flag() = true;
}

/// @brief Mutex für zusammenhängende Zeilen auf stderr.
inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

/**
 * @brief Gibt eine Logzeile aus (syslog oder stderr).
 * @param prio syslog-Priorität (LOG_INFO, ...)
 * @param level Leveltext für stderr ("INFO", ...)
 * @param fmt printf-Format
 * @param ap Argumente zu @p fmt
 */
inline void vlog(int prio, const char* level, const char* fmt, va_list ap) {
    if (log_syslog_flag()) {
        vsyslog(prio, fmt, ap);
        return;
    }
    std::lock_guard<std::mutex> lk(log_mutex());
    std::time_t now = std::time(nullptr);
    constexpr size_t kTimestampLen = sizeof("YYYY-MM-DD HH:MM:SS");  // inkl. Nullbyte
    char ts[kTimestampLen];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "%s [%s] ", ts, level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

/// Erzeugt log_<level>(fmt, ...) und log_<level>(std::string) für ein Level.
#define EW_LOG_FN(name, prio, level)               \
    inline void name(const char* fmt, ...) {       \
        va_list ap;                                \
        va_start(ap, fmt);                         \
        vlog(prio, level, fmt, ap);                \
        va_end(ap);                                \
    }                                              \
    inline void name(const std::string& s) { name("%s", s.c_str()); }

EW_LOG_FN(log_debug, LOG_DEBUG, "DEBUG")
EW_LOG_FN(log_info, LOG_INFO, "INFO")
EW_LOG_FN(log_warn, LOG_WARNING, "WARN")
EW_LOG_FN(log_error, LOG_ERR, "ERROR")

#undef EW_LOG_FN

} // namespace ew
