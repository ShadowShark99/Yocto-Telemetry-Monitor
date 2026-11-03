// SPDX-License-Identifier: MIT
//
// Minimal logger. Under systemd, stderr is connected to the journal and the
// "<N>" prefix (sd-daemon(3)) sets each line's priority, so `journalctl -p
// warning` works without linking libsystemd. On a terminal it prints a local
// timestamp and level instead.
#pragma once

#include "telemetry/config.hpp"

namespace telemetry::log {

/// Detects whether stderr is the journal. Call once at startup.
void init();

void set_level(LogLevel level) noexcept;
LogLevel level() noexcept;
bool enabled(LogLevel level) noexcept;

void write(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define TLOG_DEBUG(...) ::telemetry::log::write(::telemetry::LogLevel::Debug, __VA_ARGS__)
#define TLOG_INFO(...) ::telemetry::log::write(::telemetry::LogLevel::Info, __VA_ARGS__)
#define TLOG_WARN(...) ::telemetry::log::write(::telemetry::LogLevel::Warning, __VA_ARGS__)
#define TLOG_ERROR(...) ::telemetry::log::write(::telemetry::LogLevel::Error, __VA_ARGS__)

}  // namespace telemetry::log
