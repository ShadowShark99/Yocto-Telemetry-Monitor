// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>

#include "telemetry/config.hpp"

using namespace std::chrono_literals;
using telemetry::Config;
using telemetry::LogLevel;
using telemetry::parse_config;
using telemetry::validate;

namespace {

bool any_error_contains(const std::vector<std::string>& errors, const std::string& needle) {
    for (const auto& e : errors) {
        if (e.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(Config, DefaultsAreValid) {
    const Config config;
    EXPECT_EQ(config.udp_port, 5500);
    EXPECT_TRUE(config.serial_device.empty());
    EXPECT_TRUE(validate(config).empty());
}

TEST(Config, ParsesEveryKey) {
    const auto result = parse_config(R"(
# comment
; also a comment
udp_bind          = 127.0.0.1
udp_port          = 6000
serial_device     = /dev/ttyUSB0
serial_baud       = 9600
temp_high_c       = 55.5
temp_low_c        = -5
humidity_high_pct = 80
voltage_low_v     = 3.1
stale_timeout_ms  = 2500
report_interval_s = 30
status_file       = /tmp/status.json
max_sensors       = 8
log_level         = debug
)");
    ASSERT_TRUE(result.ok()) << result.errors.front();
    const Config& c = result.config;
    EXPECT_EQ(c.udp_bind_address, "127.0.0.1");
    EXPECT_EQ(c.udp_port, 6000);
    EXPECT_EQ(c.serial_device, "/dev/ttyUSB0");
    EXPECT_EQ(c.serial_baud, 9600U);
    EXPECT_DOUBLE_EQ(c.thresholds.temp_high_c, 55.5);
    EXPECT_DOUBLE_EQ(c.thresholds.temp_low_c, -5.0);
    EXPECT_DOUBLE_EQ(c.thresholds.humidity_high_pct, 80.0);
    EXPECT_DOUBLE_EQ(c.thresholds.voltage_low_v, 3.1);
    EXPECT_EQ(c.thresholds.stale_after, 2500ms);
    EXPECT_EQ(c.report_interval, 30s);
    EXPECT_EQ(c.status_file, "/tmp/status.json");
    EXPECT_EQ(c.max_sensors, 8U);
    EXPECT_EQ(c.log_level, LogLevel::Debug);
}

TEST(Config, HandlesInlineCommentsQuotesAndCrLf) {
    const auto result = parse_config("udp_port = 7000   # local test port\r\nstatus_file = \"\"\r\n");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.udp_port, 7000);
    EXPECT_TRUE(result.config.status_file.empty());
}

TEST(Config, KeepsDefaultsForKeysNotMentioned) {
    Config base;
    base.serial_device = "/dev/ttyS1";
    const auto result = parse_config("udp_port = 0\n", base);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.serial_device, "/dev/ttyS1");
    EXPECT_EQ(result.config.udp_port, 0);
}

TEST(Config, ReportsUnknownKeysWithLineNumbers) {
    const auto result = parse_config("udp_port = 1\n\nfrobnicate = yes\n");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors[0].find("line 3"), std::string::npos);
    EXPECT_NE(result.errors[0].find("frobnicate"), std::string::npos);
}

TEST(Config, RejectsInvalidValuesAndKeepsDefaults) {
    const auto result = parse_config(
        "udp_port = 70000\n"
        "serial_baud = 12345\n"
        "temp_high_c = hot\n"
        "log_level = loud\n"
        "missing equals sign\n");
    EXPECT_EQ(result.errors.size(), 5U);
    EXPECT_EQ(result.config.udp_port, 5500);
    EXPECT_EQ(result.config.serial_baud, 115200U);
    EXPECT_TRUE(any_error_contains(result.errors, "line 5"));
}

TEST(Config, ValidateRequiresAnInput) {
    Config c;
    c.udp_port = 0;
    EXPECT_TRUE(any_error_contains(validate(c), "no input configured"));
    c.serial_device = "/dev/ttyUSB0";
    EXPECT_TRUE(validate(c).empty());
}

TEST(Config, ValidateChecksThresholdOrder) {
    Config c;
    c.thresholds.temp_low_c = 80.0;
    EXPECT_TRUE(any_error_contains(validate(c), "temp_low_c"));
}

TEST(Config, LogLevelNames) {
    EXPECT_EQ(telemetry::parse_log_level("warn"), LogLevel::Warning);
    EXPECT_EQ(telemetry::parse_log_level("error"), LogLevel::Error);
    EXPECT_FALSE(telemetry::parse_log_level("INFO").has_value());
}

TEST(Config, MissingFileIsAnError) {
    const auto result = telemetry::load_config_file("/nonexistent/telemetryd.conf");
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.errors[0].find("cannot open"), std::string::npos);
}
