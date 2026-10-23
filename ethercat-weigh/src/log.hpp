#pragma once

#include <syslog.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace ew {

// Logausgabe
//   Konsole (Default): Zeitstempel + Level nach stderr
//   Syslog (--syslog): syslog(3), Facility LOG_DAEMON, Priorität je Level
//                      (INFO -> daemon.info, WARN -> daemon.warn, ERROR -> daemon.err)
// Unter procd wird --syslog verwendet; die Zuordnung stdout -> daemon.info,
// stderr -> daemon.err von procd kennt keine Level.

inline std::atomic<bool>& log_syslog_flag() {
    static std::atomic<bool> f{false};
    return f;
}

inline void log_use_syslog(const char* ident) {
    openlog(ident, LOG_PID, LOG_DAEMON);
    log_syslog_flag() = true;
}

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

inline void vlog(int prio, const char* level, const char* fmt, va_list ap) {
    if (log_syslog_flag()) {
        vsyslog(prio, fmt, ap);
        return;
    }
    std::lock_guard<std::mutex> lk(log_mutex());
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "%s [%s] ", ts, level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

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
