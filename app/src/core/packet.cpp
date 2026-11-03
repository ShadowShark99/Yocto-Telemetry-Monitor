// SPDX-License-Identifier: MIT
#include "telemetry/packet.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace telemetry {
namespace {

constexpr std::string_view kPacketType = "TLM";
constexpr std::size_t kFieldCount = 6;  // type, id, seq, temp, humidity, voltage
constexpr std::size_t kMaxNumberLength = 24;

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// Splits on ',' into `out` and returns the number of fields found, which may
// be larger than out.size() (the extra fields are not stored).
std::size_t split_fields(std::string_view payload, std::array<std::string_view, kFieldCount>& out) {
    std::size_t count = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = payload.find(',', start);
        const std::size_t length = comma == std::string_view::npos ? std::string_view::npos : comma - start;
        if (count < out.size()) {
            out[count] = payload.substr(start, length);
        }
        ++count;
        if (comma == std::string_view::npos) {
            return count;
        }
        start = comma + 1;
    }
}

bool parse_u32(std::string_view text, std::uint32_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

// strtod is used instead of std::from_chars<double> because not every
// standard library shipped with embedded toolchains implements the floating
// point overload. The character filter rejects inf/nan/hex forms and leading
// whitespace, which strtod would otherwise accept. The daemon never calls
// setlocale(), so the decimal separator is always '.'.
bool parse_double(std::string_view text, double& out) noexcept {
    if (text.empty() || text.size() > kMaxNumberLength) {
        return false;
    }
    const bool allowed = std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
    });
    if (!allowed) {
        return false;
    }
    std::array<char, kMaxNumberLength + 1> buffer{};
    text.copy(buffer.data(), text.size());
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(buffer.data(), &end);
    if (errno == ERANGE || end != buffer.data() + text.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool in_range(double value, double low, double high) noexcept {
    return value >= low && value <= high;
}

}  // namespace

const char* to_string(ParseError error) noexcept {
    switch (error) {
        case ParseError::Empty: return "empty";
        case ParseError::MissingStart: return "missing_start";
        case ParseError::MissingChecksum: return "missing_checksum";
        case ParseError::BadChecksumFormat: return "bad_checksum_format";
        case ParseError::ChecksumMismatch: return "checksum_mismatch";
        case ParseError::UnknownType: return "unknown_type";
        case ParseError::WrongFieldCount: return "wrong_field_count";
        case ParseError::BadSensorId: return "bad_sensor_id";
        case ParseError::BadNumber: return "bad_number";
        case ParseError::OutOfRange: return "out_of_range";
    }
    return "unknown";
}

std::uint8_t checksum(std::string_view payload) noexcept {
    std::uint8_t sum = 0;
    for (const char c : payload) {
        sum = static_cast<std::uint8_t>(sum ^ static_cast<std::uint8_t>(c));
    }
    return sum;
}

bool is_valid_sensor_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxSensorIdLength) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
               c == '_';
    });
}

ParseResult parse_packet(std::string_view line) {
    line = trim(line);
    if (line.empty()) {
        return ParseError::Empty;
    }
    if (line.front() != '$') {
        return ParseError::MissingStart;
    }

    // line[0] is '$', so a '*' found by rfind is always at index >= 1.
    const std::size_t star = line.rfind('*');
    if (star == std::string_view::npos) {
        return ParseError::MissingChecksum;
    }
    const std::string_view payload = line.substr(1, star - 1);
    const std::string_view checksum_text = line.substr(star + 1);
    if (checksum_text.size() != 2) {
        return ParseError::BadChecksumFormat;
    }
    const int high = hex_value(checksum_text[0]);
    const int low = hex_value(checksum_text[1]);
    if (high < 0 || low < 0) {
        return ParseError::BadChecksumFormat;
    }
    if (checksum(payload) != static_cast<std::uint8_t>((high << 4) | low)) {
        return ParseError::ChecksumMismatch;
    }

    std::array<std::string_view, kFieldCount> fields{};
    const std::size_t field_count = split_fields(payload, fields);
    if (fields[0] != kPacketType) {
        return ParseError::UnknownType;
    }
    if (field_count != kFieldCount) {
        return ParseError::WrongFieldCount;
    }
    if (!is_valid_sensor_id(fields[1])) {
        return ParseError::BadSensorId;
    }

    Reading reading;
    if (!parse_u32(fields[2], reading.sequence) || !parse_double(fields[3], reading.temperature_c) ||
        !parse_double(fields[4], reading.humidity_pct) || !parse_double(fields[5], reading.voltage_v)) {
        return ParseError::BadNumber;
    }
    if (!in_range(reading.temperature_c, kMinTemperatureC, kMaxTemperatureC) ||
        !in_range(reading.humidity_pct, kMinHumidityPct, kMaxHumidityPct) ||
        !in_range(reading.voltage_v, kMinVoltageV, kMaxVoltageV)) {
        return ParseError::OutOfRange;
    }
    reading.sensor_id = std::string(fields[1]);
    return reading;
}

std::string format_packet(const Reading& reading) {
    std::array<char, 128> payload{};
    const int written = std::snprintf(payload.data(), payload.size(), "TLM,%s,%u,%.2f,%.1f,%.2f",
                                      reading.sensor_id.c_str(), static_cast<unsigned>(reading.sequence),
                                      reading.temperature_c, reading.humidity_pct, reading.voltage_v);
    if (written < 0) {
        return {};
    }
    const std::string_view body(payload.data(), std::min(static_cast<std::size_t>(written), payload.size() - 1));

    std::array<char, 4> tail{};
    std::snprintf(tail.data(), tail.size(), "*%02X", static_cast<unsigned>(checksum(body)));

    std::string line;
    line.reserve(body.size() + 4);
    line += '$';
    line += body;
    line += tail.data();
    return line;
}

}  // namespace telemetry
