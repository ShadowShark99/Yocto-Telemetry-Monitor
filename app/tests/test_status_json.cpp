// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "telemetry/health_monitor.hpp"
#include "telemetry/status_json.hpp"

using namespace std::chrono_literals;
using telemetry::Clock;
using telemetry::HealthMonitor;
using telemetry::Reading;

TEST(StatusJson, EmptySnapshot) {
    HealthMonitor monitor;
    const std::string json = telemetry::to_json(monitor.snapshot(Clock::now()), 5s, "1.2.3");
    EXPECT_NE(json.find("\"version\": \"1.2.3\""), std::string::npos);
    EXPECT_NE(json.find("\"uptime_s\": 5"), std::string::npos);
    EXPECT_NE(json.find("\"status\": \"no_data\""), std::string::npos);
    EXPECT_NE(json.find("\"sensors\": []"), std::string::npos);
    EXPECT_NE(json.find("\"parse_errors\": {}"), std::string::npos);
}

TEST(StatusJson, IncludesSensorsAlarmsAndErrorCounts) {
    HealthMonitor monitor;
    const auto t0 = Clock::now();
    Reading hot;
    hot.sensor_id = "env-01";
    hot.sequence = 7;
    hot.temperature_c = 85.0;
    hot.humidity_pct = 40.0;
    hot.voltage_v = 3.3;
    monitor.on_reading(hot, t0);
    monitor.on_parse_error(telemetry::ParseError::ChecksumMismatch);

    const std::string json = telemetry::to_json(monitor.snapshot(t0 + 250ms), 60s, "1.0.0");
    EXPECT_NE(json.find("\"status\": \"degraded\""), std::string::npos);
    EXPECT_NE(json.find("\"id\": \"env-01\""), std::string::npos);
    EXPECT_NE(json.find("\"alarms\": [\"over_temperature\"]"), std::string::npos);
    EXPECT_NE(json.find("\"age_ms\": 250"), std::string::npos);
    EXPECT_NE(json.find("\"temperature_c\": 85.00"), std::string::npos);
    EXPECT_NE(json.find("\"parse_errors\": {\"checksum_mismatch\": 1}"), std::string::npos);
    EXPECT_NE(json.find("\"packets_total\": 1,"), std::string::npos);
}

TEST(StatusJson, EscapesStrings) {
    EXPECT_EQ(telemetry::json_escape("a\"b\\c\n"), "a\\\"b\\\\c\\n");
    EXPECT_EQ(telemetry::json_escape(std::string("\x01", 1)), "\\u0001");
}
