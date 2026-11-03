// SPDX-License-Identifier: MIT
#include "telemetry/config.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace telemetry {
namespace {

constexpr std::array<std::uint32_t, 8> kSupportedBauds = {9600,   19200,  38400,  57600,
                                                          115200, 230400, 460800, 921600};

std::string_view trim(std::string_view text) noexcept {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Removes a trailing " # comment" (the '#' must follow whitespace, so paths
// containing '#' still work when quoted) and surrounding double quotes.
std::string_view clean_value(std::string_view value) {
    if (!value.empty() && value.front() == '"') {
        const std::size_t close = value.find('"', 1);
        if (close != std::string_view::npos) {
            return value.substr(1, close - 1);
        }
        return value.substr(1);
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        if (value[i] == '#' && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
            return trim(value.substr(0, i));
        }
    }
    return value;
}

template <typename Int>
bool parse_int(std::string_view text, Int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

bool parse_number(std::string_view text, double& out) {
    if (text.empty() || text.size() > 32) {
        return false;
    }
    const std::string copy(text);
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (errno == ERANGE || end != copy.c_str() + copy.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

class Parser {
public:
    explicit Parser(Config& config, std::vector<std::string>& errors) : config_(config), errors_(errors) {}

    void handle(std::size_t line_no, std::string_view key, std::string_view value) {
        line_ = line_no;
        if (key == "udp_bind") {
            config_.udp_bind_address = std::string(value);
        } else if (key == "udp_port") {
            set_int(key, value, config_.udp_port, 0, 65535);
        } else if (key == "serial_device") {
            config_.serial_device = std::string(value);
        } else if (key == "serial_baud") {
            std::uint32_t baud = 0;
            if (!parse_int(value, baud) || !is_supported_baud(baud)) {
                error(key, value, "expected one of 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600");
            } else {
                config_.serial_baud = baud;
            }
        } else if (key == "temp_high_c") {
            set_double(key, value, config_.thresholds.temp_high_c, -60.0, 150.0);
        } else if (key == "temp_low_c") {
            set_double(key, value, config_.thresholds.temp_low_c, -60.0, 150.0);
        } else if (key == "humidity_high_pct") {
            set_double(key, value, config_.thresholds.humidity_high_pct, 0.0, 100.0);
        } else if (key == "voltage_low_v") {
            set_double(key, value, config_.thresholds.voltage_low_v, 0.0, 60.0);
        } else if (key == "stale_timeout_ms") {
            std::int64_t ms = 0;
            if (set_int(key, value, ms, 100, 3'600'000)) {
                config_.thresholds.stale_after = std::chrono::milliseconds{ms};
            }
        } else if (key == "report_interval_s") {
            std::int64_t seconds = 0;
            if (set_int(key, value, seconds, 1, 86'400)) {
                config_.report_interval = std::chrono::seconds{seconds};
            }
        } else if (key == "status_file") {
            config_.status_file = std::string(value);
        } else if (key == "max_sensors") {
            set_int(key, value, config_.max_sensors, 1, 1024);
        } else if (key == "log_level") {
            const auto level = parse_log_level(value);
            if (!level) {
                error(key, value, "expected debug, info, warning or error");
            } else {
                config_.log_level = *level;
            }
        } else {
            std::ostringstream msg;
            msg << "line " << line_ << ": unknown key '" << key << "'";
            errors_.push_back(msg.str());
        }
    }

private:
    template <typename Int>
    bool set_int(std::string_view key, std::string_view value, Int& out, long long low, long long high) {
        long long parsed = 0;
        if (!parse_int(value, parsed) || parsed < low || parsed > high) {
            std::ostringstream expected;
            expected << "expected an integer in " << low << ".." << high;
            error(key, value, expected.str());
            return false;
        }
        out = static_cast<Int>(parsed);
        return true;
    }

    void set_double(std::string_view key, std::string_view value, double& out, double low, double high) {
        double parsed = 0.0;
        if (!parse_number(value, parsed) || parsed < low || parsed > high) {
            std::ostringstream expected;
            expected << "expected a number in " << low << ".." << high;
            error(key, value, expected.str());
            return;
        }
        out = parsed;
    }

    void error(std::string_view key, std::string_view value, const std::string& expected) {
        std::ostringstream msg;
        msg << "line " << line_ << ": invalid value '" << value << "' for " << key << " (" << expected << ")";
        errors_.push_back(msg.str());
    }

    Config& config_;
    std::vector<std::string>& errors_;
    std::size_t line_ = 0;
};

}  // namespace

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
    }
    return "unknown";
}

std::optional<LogLevel> parse_log_level(std::string_view text) noexcept {
    if (text == "debug") return LogLevel::Debug;
    if (text == "info") return LogLevel::Info;
    if (text == "warning" || text == "warn") return LogLevel::Warning;
    if (text == "error") return LogLevel::Error;
    return std::nullopt;
}

bool is_supported_baud(std::uint32_t baud) noexcept {
    for (const auto supported : kSupportedBauds) {
        if (supported == baud) {
            return true;
        }
    }
    return false;
}

ConfigResult parse_config(std::string_view text, const Config& defaults) {
    ConfigResult result{defaults, {}};
    Parser parser(result.config, result.errors);

    std::size_t line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            std::ostringstream msg;
            msg << "line " << line_no << ": expected 'key = value'";
            result.errors.push_back(msg.str());
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = clean_value(trim(line.substr(equals + 1)));
        parser.handle(line_no, key, value);
    }
    return result;
}

ConfigResult load_config_file(const std::string& path, const Config& defaults) {
    std::ifstream file(path);
    if (!file) {
        ConfigResult result{defaults, {}};
        result.errors.push_back("cannot open " + path + ": " + std::strerror(errno));
        return result;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    ConfigResult result = parse_config(contents.str(), defaults);
    for (auto& message : result.errors) {
        message = path + ": " + message;
    }
    return result;
}

std::vector<std::string> validate(const Config& config) {
    std::vector<std::string> errors;
    if (config.udp_port == 0 && config.serial_device.empty()) {
        errors.emplace_back("no input configured: set udp_port and/or serial_device");
    }
    if (config.thresholds.temp_low_c >= config.thresholds.temp_high_c) {
        errors.emplace_back("temp_low_c must be lower than temp_high_c");
    }
    if (config.udp_port != 0 && config.udp_bind_address.empty()) {
        errors.emplace_back("udp_bind must not be empty when udp_port is set");
    }
    return errors;
}

}  // namespace telemetry
