#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace ew {

// Minimales Logging nach stderr (unter procd -> logd, abrufbar mit `logread`)
inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

inline void vlog(const char* level, const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> lk(log_mutex());
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "%s [%s] ", ts, level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

#define EW_LOG_FN(name, level)                     \
    inline void name(const char* fmt, ...) {       \
        va_list ap;                                \
        va_start(ap, fmt);                         \
        vlog(level, fmt, ap);                      \
        va_end(ap);                                \
    }                                              \
    inline void name(const std::string& s) { name("%s", s.c_str()); }

EW_LOG_FN(log_info, "INFO")
EW_LOG_FN(log_warn, "WARN")
EW_LOG_FN(log_error, "ERROR")

#undef EW_LOG_FN

} // namespace ew
