// SPDX-License-Identifier: MIT
//
// Per-sensor health tracking: threshold alarms with hysteresis, stale-sensor
// detection, packet-loss accounting from sequence numbers, and summary stats.
//
// The monitor is pure logic. Time is passed in by the caller, which keeps it
// deterministic and unit-testable without sleeping.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "telemetry/packet.hpp"

namespace telemetry {

using Clock = std::chrono::steady_clock;

struct Thresholds {
    double temp_high_c = 70.0;
    double temp_low_c = -10.0;
    double humidity_high_pct = 90.0;
    double voltage_low_v = 3.0;

    // An alarm clears only once the value is back inside the limit by this
    // margin, so a reading hovering at the threshold does not flap.
    double temp_hysteresis_c = 1.0;
    double humidity_hysteresis_pct = 2.0;
    double voltage_hysteresis_v = 0.05;

    std::chrono::milliseconds stale_after{5000};
};

enum class AlertKind {
    OverTemperature,
    UnderTemperature,
    HighHumidity,
    LowVoltage,
    Stale,
    PacketLoss,
    SequenceReset,
    NewSensor,
    SensorRejected,
};

/// Raised/Cleared describe a condition that persists; Event is one-off.
enum class AlertType { Raised, Cleared, Event };

struct Alert {
    AlertKind kind;
    AlertType type;
    std::string sensor_id;
    std::string message;
};

const char* to_string(AlertKind kind) noexcept;
const char* to_string(AlertType type) noexcept;

struct SensorStatus {
    std::string sensor_id;
    Reading last;
    std::uint64_t packets = 0;
    std::uint64_t missed = 0;  // packets inferred lost from sequence gaps
    std::uint64_t resets = 0;  // sequence restarts (node rebooted)
    double temp_min_c = 0.0;
    double temp_max_c = 0.0;
    double temp_mean_c = 0.0;
    std::chrono::milliseconds age{0};  // time since the last packet
    bool stale = false;
    bool over_temperature = false;
    bool under_temperature = false;
    bool high_humidity = false;
    bool low_voltage = false;

    bool healthy() const noexcept;
    std::vector<AlertKind> active_alarms() const;
};

enum class OverallStatus { NoData, Ok, Degraded };
const char* to_string(OverallStatus status) noexcept;

struct HealthSnapshot {
    OverallStatus status = OverallStatus::NoData;
    std::vector<SensorStatus> sensors;  // sorted by sensor_id
    std::uint64_t packets_total = 0;
    std::uint64_t parse_errors_total = 0;
    std::array<std::uint64_t, kParseErrorCount> parse_errors{};  // indexed by ParseError
    std::uint64_t rejected_packets = 0;  // dropped because the sensor table was full
};

class HealthMonitor {
public:
    /// A sequence jump forward of at least this many packets is treated as a
    /// node restart rather than as loss.
    static constexpr std::uint32_t kMaxSequenceGap = 1000;

    explicit HealthMonitor(Thresholds thresholds = {}, std::size_t max_sensors = 32);

    void set_thresholds(const Thresholds& thresholds) { thresholds_ = thresholds; }
    const Thresholds& thresholds() const noexcept { return thresholds_; }

    /// Affects only sensors seen for the first time from now on.
    void set_max_sensors(std::size_t max_sensors) noexcept { max_sensors_ = max_sensors; }

    /// Feeds one valid reading. Returns the alerts it caused.
    std::vector<Alert> on_reading(const Reading& reading, Clock::time_point now);

    /// Counts a line the parser rejected.
    void on_parse_error(ParseError error) noexcept;

    /// Marks sensors stale once they have been silent for stale_after.
    std::vector<Alert> check_staleness(Clock::time_point now);

    HealthSnapshot snapshot(Clock::time_point now) const;

    std::size_t sensor_count() const noexcept { return sensors_.size(); }

private:
    struct SensorState {
        Reading last;
        Clock::time_point last_seen{};
        std::uint64_t packets = 0;
        std::uint64_t missed = 0;
        std::uint64_t resets = 0;
        double temp_min_c = 0.0;
        double temp_max_c = 0.0;
        double temp_sum_c = 0.0;
        bool stale = false;
        bool over_temperature = false;
        bool under_temperature = false;
        bool high_humidity = false;
        bool low_voltage = false;
    };

    void check_sequence(SensorState& state, const Reading& reading, std::vector<Alert>& alerts);
    void check_thresholds(SensorState& state, const Reading& reading, std::vector<Alert>& alerts) const;

    Thresholds thresholds_;
    std::size_t max_sensors_;
    std::map<std::string, SensorState, std::less<>> sensors_;
    std::uint64_t packets_total_ = 0;
    std::array<std::uint64_t, kParseErrorCount> parse_errors_{};
    std::uint64_t rejected_packets_ = 0;
};

}  // namespace telemetry
