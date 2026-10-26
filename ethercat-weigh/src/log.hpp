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
 * @brief Log output.
 *
 * - Console (default): timestamp + level to stderr
 * - Syslog (--syslog): syslog(3), facility LOG_DAEMON, priority per level
 *   (INFO -> daemon.info, WARN -> daemon.warn, ERROR -> daemon.err)
 *
 * Under procd, --syslog is used; procd's mapping stdout -> daemon.info,
 * stderr -> daemon.err knows no levels.
 */

/// @brief Switch "output via syslog" (process-wide).
inline std::atomic<bool>& log_syslog_flag() {
    static std::atomic<bool> f{false};
    return f;
}

/**
 * @brief Switches the log output to syslog(3).
 * @param ident identifier for openlog(3), e.g. "ethercat-weigh"
 */
inline void log_use_syslog(const char* ident) {
    openlog(ident, LOG_PID, LOG_DAEMON);
    log_syslog_flag() = true;
}

/// @brief Mutex for contiguous lines on stderr.
inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

/**
 * @brief Writes a log line (syslog or stderr).
 * @param prio syslog priority (LOG_INFO, ...)
 * @param level level text for stderr ("INFO", ...)
 * @param fmt printf format
 * @param ap arguments for @p fmt
 */
inline void vlog(int prio, const char* level, const char* fmt, va_list ap) {
    if (log_syslog_flag()) {
        vsyslog(prio, fmt, ap);
        return;
    }
    std::lock_guard<std::mutex> lk(log_mutex());
    std::time_t now = std::time(nullptr);
    constexpr size_t kTimestampLen = sizeof("YYYY-MM-DD HH:MM:SS");  // incl. null byte
    char ts[kTimestampLen];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "%s [%s] ", ts, level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

/// Generates log_<level>(fmt, ...) and log_<level>(std::string) for one level.
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
