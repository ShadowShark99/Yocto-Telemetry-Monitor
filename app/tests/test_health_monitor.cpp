// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "telemetry/health_monitor.hpp"

using namespace std::chrono_literals;
using telemetry::Alert;
using telemetry::AlertKind;
using telemetry::AlertType;
using telemetry::Clock;
using telemetry::HealthMonitor;
using telemetry::OverallStatus;
using telemetry::Reading;
using telemetry::Thresholds;

namespace {

Reading make_reading(const std::string& id, std::uint32_t seq, double temp = 22.0, double humidity = 45.0,
                     double voltage = 3.3) {
    Reading r;
    r.sensor_id = id;
    r.sequence = seq;
    r.temperature_c = temp;
    r.humidity_pct = humidity;
    r.voltage_v = voltage;
    return r;
}

bool has(const std::vector<Alert>& alerts, AlertKind kind, AlertType type) {
    return std::any_of(alerts.begin(), alerts.end(),
                       [&](const Alert& a) { return a.kind == kind && a.type == type; });
}

std::size_t count(const std::vector<Alert>& alerts, AlertKind kind) {
    return static_cast<std::size_t>(
        std::count_if(alerts.begin(), alerts.end(), [&](const Alert& a) { return a.kind == kind; }));
}

class HealthMonitorTest : public ::testing::Test {
protected:
    Clock::time_point t0 = Clock::time_point{} + 1000s;
    HealthMonitor monitor{Thresholds{}, 4};
};

}  // namespace

TEST_F(HealthMonitorTest, StartsWithNoData) {
    const auto snap = monitor.snapshot(t0);
    EXPECT_EQ(snap.status, OverallStatus::NoData);
    EXPECT_TRUE(snap.sensors.empty());
}

TEST_F(HealthMonitorTest, RegistersNewSensorsOnce) {
    auto alerts = monitor.on_reading(make_reading("a", 1), t0);
    EXPECT_TRUE(has(alerts, AlertKind::NewSensor, AlertType::Event));
    alerts = monitor.on_reading(make_reading("a", 2), t0 + 1s);
    EXPECT_TRUE(alerts.empty());

    const auto snap = monitor.snapshot(t0 + 1500ms);
    ASSERT_EQ(snap.sensors.size(), 1U);
    EXPECT_EQ(snap.sensors[0].packets, 2U);
    EXPECT_EQ(snap.sensors[0].age, 500ms);
    EXPECT_EQ(snap.status, OverallStatus::Ok);
}

TEST_F(HealthMonitorTest, OverTemperatureRaisesOnceAndClearsWithHysteresis) {
    monitor.on_reading(make_reading("a", 1, 25.0), t0);

    auto alerts = monitor.on_reading(make_reading("a", 2, 75.0), t0 + 1s);
    EXPECT_TRUE(has(alerts, AlertKind::OverTemperature, AlertType::Raised));

    // Still hot: no repeated alarm.
    alerts = monitor.on_reading(make_reading("a", 3, 80.0), t0 + 2s);
    EXPECT_EQ(count(alerts, AlertKind::OverTemperature), 0U);
    EXPECT_EQ(monitor.snapshot(t0 + 2s).status, OverallStatus::Degraded);

    // Just under the limit but inside the 1 C hysteresis band: stays raised.
    alerts = monitor.on_reading(make_reading("a", 4, 69.5), t0 + 3s);
    EXPECT_EQ(count(alerts, AlertKind::OverTemperature), 0U);

    alerts = monitor.on_reading(make_reading("a", 5, 68.0), t0 + 4s);
    EXPECT_TRUE(has(alerts, AlertKind::OverTemperature, AlertType::Cleared));
    EXPECT_EQ(monitor.snapshot(t0 + 4s).status, OverallStatus::Ok);
}

TEST_F(HealthMonitorTest, LowVoltageAndHighHumidityAlarms) {
    auto alerts = monitor.on_reading(make_reading("a", 1, 22.0, 95.0, 2.8), t0);
    EXPECT_TRUE(has(alerts, AlertKind::LowVoltage, AlertType::Raised));
    EXPECT_TRUE(has(alerts, AlertKind::HighHumidity, AlertType::Raised));

    const auto snap = monitor.snapshot(t0);
    ASSERT_EQ(snap.sensors.size(), 1U);
    const auto alarms = snap.sensors[0].active_alarms();
    EXPECT_EQ(alarms.size(), 2U);

    alerts = monitor.on_reading(make_reading("a", 2, 22.0, 50.0, 3.3), t0 + 1s);
    EXPECT_TRUE(has(alerts, AlertKind::LowVoltage, AlertType::Cleared));
    EXPECT_TRUE(has(alerts, AlertKind::HighHumidity, AlertType::Cleared));
}

TEST_F(HealthMonitorTest, UnderTemperatureAlarm) {
    auto alerts = monitor.on_reading(make_reading("a", 1, -15.0), t0);
    EXPECT_TRUE(has(alerts, AlertKind::UnderTemperature, AlertType::Raised));
    alerts = monitor.on_reading(make_reading("a", 2, -9.5), t0 + 1s);  // inside hysteresis
    EXPECT_EQ(count(alerts, AlertKind::UnderTemperature), 0U);
    alerts = monitor.on_reading(make_reading("a", 3, -8.0), t0 + 2s);
    EXPECT_TRUE(has(alerts, AlertKind::UnderTemperature, AlertType::Cleared));
}

TEST_F(HealthMonitorTest, DetectsStaleSensorsAndRecovery) {
    monitor.on_reading(make_reading("a", 1), t0);
    EXPECT_TRUE(monitor.check_staleness(t0 + 4900ms).empty());

    auto alerts = monitor.check_staleness(t0 + 5s);
    EXPECT_TRUE(has(alerts, AlertKind::Stale, AlertType::Raised));
    EXPECT_TRUE(monitor.check_staleness(t0 + 10s).empty()) << "stale is reported once";
    EXPECT_EQ(monitor.snapshot(t0 + 10s).status, OverallStatus::Degraded);

    alerts = monitor.on_reading(make_reading("a", 2), t0 + 12s);
    EXPECT_TRUE(has(alerts, AlertKind::Stale, AlertType::Cleared));
    EXPECT_EQ(monitor.snapshot(t0 + 12s).status, OverallStatus::Ok);
}

TEST_F(HealthMonitorTest, CountsMissingPacketsFromSequenceGaps) {
    monitor.on_reading(make_reading("a", 10), t0);
    auto alerts = monitor.on_reading(make_reading("a", 14), t0 + 1s);
    EXPECT_TRUE(has(alerts, AlertKind::PacketLoss, AlertType::Event));
    EXPECT_EQ(monitor.snapshot(t0).sensors[0].missed, 3U);
}

TEST_F(HealthMonitorTest, HandlesSequenceWrapAround) {
    monitor.on_reading(make_reading("a", 0xFFFFFFFFU), t0);
    const auto alerts = monitor.on_reading(make_reading("a", 0), t0 + 1s);
    EXPECT_TRUE(alerts.empty());
    EXPECT_EQ(monitor.snapshot(t0).sensors[0].missed, 0U);
}

TEST_F(HealthMonitorTest, TreatsBackwardsSequenceAsRestart) {
    monitor.on_reading(make_reading("a", 500), t0);
    const auto alerts = monitor.on_reading(make_reading("a", 0), t0 + 1s);
    EXPECT_TRUE(has(alerts, AlertKind::SequenceReset, AlertType::Event));
    const auto snap = monitor.snapshot(t0);
    EXPECT_EQ(snap.sensors[0].resets, 1U);
    EXPECT_EQ(snap.sensors[0].missed, 0U);
}

TEST_F(HealthMonitorTest, RejectsSensorsBeyondTheTableLimit) {
    for (int i = 0; i < 4; ++i) {
        monitor.on_reading(make_reading("s" + std::to_string(i), 0), t0);
    }
    auto alerts = monitor.on_reading(make_reading("intruder", 0), t0);
    EXPECT_TRUE(has(alerts, AlertKind::SensorRejected, AlertType::Event));
    alerts = monitor.on_reading(make_reading("intruder2", 0), t0);
    EXPECT_TRUE(alerts.empty()) << "rejections are rate limited";

    const auto snap = monitor.snapshot(t0);
    EXPECT_EQ(snap.sensors.size(), 4U);
    EXPECT_EQ(snap.rejected_packets, 2U);
    EXPECT_EQ(snap.packets_total, 6U);
}

TEST_F(HealthMonitorTest, TracksTemperatureStatistics) {
    monitor.on_reading(make_reading("a", 1, 20.0), t0);
    monitor.on_reading(make_reading("a", 2, 30.0), t0);
    monitor.on_reading(make_reading("a", 3, 25.0), t0);
    const auto s = monitor.snapshot(t0).sensors.at(0);
    EXPECT_DOUBLE_EQ(s.temp_min_c, 20.0);
    EXPECT_DOUBLE_EQ(s.temp_max_c, 30.0);
    EXPECT_DOUBLE_EQ(s.temp_mean_c, 25.0);
    EXPECT_DOUBLE_EQ(s.last.temperature_c, 25.0);
}

TEST_F(HealthMonitorTest, CountsParseErrorsByKind) {
    monitor.on_parse_error(telemetry::ParseError::ChecksumMismatch);
    monitor.on_parse_error(telemetry::ParseError::ChecksumMismatch);
    monitor.on_parse_error(telemetry::ParseError::BadNumber);
    const auto snap = monitor.snapshot(t0);
    EXPECT_EQ(snap.parse_errors_total, 3U);
    EXPECT_EQ(snap.parse_errors[static_cast<std::size_t>(telemetry::ParseError::ChecksumMismatch)], 2U);
}

TEST_F(HealthMonitorTest, NewThresholdsApplyToTheNextReading) {
    monitor.on_reading(make_reading("a", 1, 50.0), t0);
    Thresholds stricter;
    stricter.temp_high_c = 40.0;
    monitor.set_thresholds(stricter);
    const auto alerts = monitor.on_reading(make_reading("a", 2, 50.0), t0 + 1s);
    EXPECT_TRUE(has(alerts, AlertKind::OverTemperature, AlertType::Raised));
}

TEST_F(HealthMonitorTest, SnapshotIsSortedBySensorId) {
    monitor.on_reading(make_reading("charlie", 0), t0);
    monitor.on_reading(make_reading("alpha", 0), t0);
    monitor.on_reading(make_reading("bravo", 0), t0);
    const auto snap = monitor.snapshot(t0);
    ASSERT_EQ(snap.sensors.size(), 3U);
    EXPECT_EQ(snap.sensors[0].sensor_id, "alpha");
    EXPECT_EQ(snap.sensors[2].sensor_id, "charlie");
}
