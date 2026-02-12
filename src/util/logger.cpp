#include "util/logger.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace sc {

static LogLevel g_level = LogLevel::INFO;

void log_set_level(LogLevel lvl) { g_level = lvl; }

void log_msg(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (lvl < g_level) return;

    const char* prefix = "?";
    switch (lvl) {
        case LogLevel::DEBUG: prefix = "DBG"; break;
        case LogLevel::INFO:  prefix = "INF"; break;
        case LogLevel::WARN:  prefix = "WRN"; break;
        case LogLevel::ERROR: prefix = "ERR"; break;
    }

    // Timestamp
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long sec  = ts.tv_sec;
    unsigned long msec = ts.tv_nsec / 1000000;

    fprintf(stderr, "[%lu.%03lu] [%s/%s] ", sec, msec, prefix, tag);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

}  // namespace sc
