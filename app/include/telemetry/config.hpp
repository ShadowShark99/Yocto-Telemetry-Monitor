// SPDX-License-Identifier: MIT
//
// Daemon configuration: a small `key = value` file, e.g.
//
//   # /etc/telemetryd/telemetryd.conf
//   udp_port      = 5500
//   serial_device = /dev/ttyUSB0
//   temp_high_c   = 70
//
// Unknown keys and bad values are reported as errors with line numbers
// rather than silently ignored.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "telemetry/health_monitor.hpp"

namespace telemetry {

enum class LogLevel { Debug, Info, Warning, Error };

const char* to_string(LogLevel level) noexcept;
std::optional<LogLevel> parse_log_level(std::string_view text) noexcept;

struct Config {
    std::string udp_bind_address = "0.0.0.0";
    std::uint16_t udp_port = 5500;  // 0 disables UDP input
    std::string serial_device;      // empty disables serial input
    std::uint32_t serial_baud = 115200;
    Thresholds thresholds;
    std::chrono::seconds report_interval{10};
    std::string status_file = "/run/telemetryd/status.json";  // empty disables
    std::size_t max_sensors = 32;
    LogLevel log_level = LogLevel::Info;
};

struct ConfigResult {
    Config config;
    std::vector<std::string> errors;
    bool ok() const noexcept { return errors.empty(); }
};

/// Parses configuration text on top of `defaults`. Only syntax and per-key
/// values are checked here; call validate() once command-line overrides
/// have been applied as well.
ConfigResult parse_config(std::string_view text, const Config& defaults = Config{});

/// Reads and parses a file. A missing or unreadable file is an error.
ConfigResult load_config_file(const std::string& path, const Config& defaults = Config{});

/// Cross-field checks on the final configuration (at least one input,
/// temp_low_c < temp_high_c, ...).
std::vector<std::string> validate(const Config& config);

bool is_supported_baud(std::uint32_t baud) noexcept;

}  // namespace telemetry
