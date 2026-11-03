// SPDX-License-Identifier: MIT
//
// Wire format for sensor packets.
//
// Each packet is one ASCII line, modelled on NMEA 0183:
//
//   $TLM,<sensor_id>,<seq>,<temp_c>,<humidity_pct>,<voltage_v>*<XX>
//
//   sensor_id     1-32 characters of [A-Za-z0-9_-]
//   seq           unsigned 32-bit counter, incremented per packet, wraps
//   temp_c        temperature in degrees Celsius
//   humidity_pct  relative humidity, 0-100
//   voltage_v     supply voltage in volts
//   XX            XOR of every byte between '$' and '*', two hex digits
//
// Example: $TLM,env-01,42,23.50,41.2,3.29*0B
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace telemetry {

/// One decoded sample from a sensor node.
struct Reading {
    std::string sensor_id;
    std::uint32_t sequence = 0;
    double temperature_c = 0.0;
    double humidity_pct = 0.0;
    double voltage_v = 0.0;
};

/// Why a line was rejected. Keep in sync with kParseErrorCount and to_string().
enum class ParseError {
    Empty,
    MissingStart,
    MissingChecksum,
    BadChecksumFormat,
    ChecksumMismatch,
    UnknownType,
    WrongFieldCount,
    BadSensorId,
    BadNumber,
    OutOfRange,
};

inline constexpr std::size_t kParseErrorCount = 10;

/// Stable snake_case name, used in logs and the status file.
const char* to_string(ParseError error) noexcept;

using ParseResult = std::variant<Reading, ParseError>;

// Physical limits the parser accepts. Values outside these are treated as a
// corrupted or faulty sensor, not as an alarm condition.
inline constexpr double kMinTemperatureC = -60.0;
inline constexpr double kMaxTemperatureC = 150.0;
inline constexpr double kMinHumidityPct = 0.0;
inline constexpr double kMaxHumidityPct = 100.0;
inline constexpr double kMinVoltageV = 0.0;
inline constexpr double kMaxVoltageV = 60.0;
inline constexpr std::size_t kMaxSensorIdLength = 32;

/// XOR of every byte in `payload` (the text between '$' and '*').
std::uint8_t checksum(std::string_view payload) noexcept;

/// Parses one line. Surrounding whitespace and a trailing CR are ignored.
ParseResult parse_packet(std::string_view line);

/// Formats a reading as a wire line, without the trailing newline.
std::string format_packet(const Reading& reading);

/// True for 1-32 characters of [A-Za-z0-9_-].
bool is_valid_sensor_id(std::string_view id) noexcept;

}  // namespace telemetry
