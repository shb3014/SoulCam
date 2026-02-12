#pragma once
// ============================================================================
// Minimal logging utility
// ============================================================================

#include <cstdio>
#include <cstdarg>

namespace sc {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

void log_set_level(LogLevel lvl);
void log_msg(LogLevel lvl, const char* tag, const char* fmt, ...);

}  // namespace sc

// Convenience macros -- tag is auto-set to the enclosing file's base name.
#define SC_LOG_DEBUG(fmt, ...) sc::log_msg(sc::LogLevel::DEBUG, "SC", fmt, ##__VA_ARGS__)
#define SC_LOG_INFO(fmt, ...)  sc::log_msg(sc::LogLevel::INFO,  "SC", fmt, ##__VA_ARGS__)
#define SC_LOG_WARN(fmt, ...)  sc::log_msg(sc::LogLevel::WARN,  "SC", fmt, ##__VA_ARGS__)
#define SC_LOG_ERROR(fmt, ...) sc::log_msg(sc::LogLevel::ERROR, "SC", fmt, ##__VA_ARGS__)
