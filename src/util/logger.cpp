#include "util/logger.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>

namespace sc {

static LogLevel g_level = LogLevel::INFO;
static FILE*    g_log_fp = nullptr;

void log_set_level(LogLevel lvl) { g_level = lvl; }

static FILE* log_fp() {
    return g_log_fp ? g_log_fp : stderr;
}

void log_redirect_stderr_quiet() {
    int saved = dup(STDERR_FILENO);
    if (saved < 0) return;
    g_log_fp = fdopen(saved, "w");
    if (!g_log_fp) { close(saved); return; }
    setvbuf(g_log_fp, nullptr, _IOLBF, 0);
    freopen("/dev/null", "w", stderr);
}

void log_msg(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (lvl < g_level) return;

    const char* prefix = "?";
    switch (lvl) {
        case LogLevel::DEBUG: prefix = "DBG"; break;
        case LogLevel::INFO:  prefix = "INF"; break;
        case LogLevel::WARN:  prefix = "WRN"; break;
        case LogLevel::ERROR: prefix = "ERR"; break;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long sec  = ts.tv_sec;
    unsigned long msec = ts.tv_nsec / 1000000;

    FILE* fp = log_fp();
    fprintf(fp, "[%lu.%03lu] [%s/%s] ", sec, msec, prefix, tag);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
}

}  // namespace sc
