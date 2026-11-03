// SPDX-License-Identifier: MIT
#include "telemetry/health_monitor.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string_view>
#include <utility>

namespace telemetry {
namespace {

__attribute__((format(printf, 1, 2))) std::string format(const char* fmt, ...) {
    std::array<char, 160> buffer{};
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);
    if (written < 0) {
        return {};
    }
    return std::string(buffer.data(), std::min(static_cast<std::size_t>(written), buffer.size() - 1));
}

Alert make_alert(AlertKind kind, AlertType type, const std::string& sensor_id, std::string message) {
    return Alert{kind, type, sensor_id, std::move(message)};
}

// Raises when `exceeded` and clears when `recovered`. The gap between the two
// conditions is the hysteresis band.
void update_condition(bool& active, bool exceeded, bool recovered, AlertKind kind, const std::string& sensor_id,
                      std::string raised_message, std::string cleared_message, std::vector<Alert>& alerts) {
    if (!active && exceeded) {
        active = true;
        alerts.push_back(make_alert(kind, AlertType::Raised, sensor_id, std::move(raised_message)));
    } else if (active && recovered) {
        active = false;
        alerts.push_back(make_alert(kind, AlertType::Cleared, sensor_id, std::move(cleared_message)));
    }
}

}  // namespace

const char* to_string(AlertKind kind) noexcept {
    switch (kind) {
        case AlertKind::OverTemperature: return "over_temperature";
        case AlertKind::UnderTemperature: return "under_temperature";
        case AlertKind::HighHumidity: return "high_humidity";
        case AlertKind::LowVoltage: return "low_voltage";
        case AlertKind::Stale: return "stale";
        case AlertKind::PacketLoss: return "packet_loss";
        case AlertKind::SequenceReset: return "sequence_reset";
        case AlertKind::NewSensor: return "new_sensor";
        case AlertKind::SensorRejected: return "sensor_rejected";
    }
    return "unknown";
}

const char* to_string(AlertType type) noexcept {
    switch (type) {
        case AlertType::Raised: return "raised";
        case AlertType::Cleared: return "cleared";
        case AlertType::Event: return "event";
    }
    return "unknown";
}

const char* to_string(OverallStatus status) noexcept {
    switch (status) {
        case OverallStatus::NoData: return "no_data";
        case OverallStatus::Ok: return "ok";
        case OverallStatus::Degraded: return "degraded";
    }
    return "unknown";
}

bool SensorStatus::healthy() const noexcept {
    return !stale && !over_temperature && !under_temperature && !high_humidity && !low_voltage;
}

std::vector<AlertKind> SensorStatus::active_alarms() const {
    std::vector<AlertKind> alarms;
    if (stale) alarms.push_back(AlertKind::Stale);
    if (over_temperature) alarms.push_back(AlertKind::OverTemperature);
    if (under_temperature) alarms.push_back(AlertKind::UnderTemperature);
    if (high_humidity) alarms.push_back(AlertKind::HighHumidity);
    if (low_voltage) alarms.push_back(AlertKind::LowVoltage);
    return alarms;
}

HealthMonitor::HealthMonitor(Thresholds thresholds, std::size_t max_sensors)
    : thresholds_(thresholds), max_sensors_(max_sensors) {}

std::vector<Alert> HealthMonitor::on_reading(const Reading& reading, Clock::time_point now) {
    std::vector<Alert> alerts;
    ++packets_total_;

    auto it = sensors_.find(reading.sensor_id);
    if (it == sensors_.end()) {
        if (sensors_.size() >= max_sensors_) {
            ++rejected_packets_;
            // Report the first rejection and then every 100th, so a flood of
            // spoofed IDs cannot flood the log as well.
            if (rejected_packets_ % 100 == 1) {
                alerts.push_back(make_alert(AlertKind::SensorRejected, AlertType::Event, reading.sensor_id,
                                            format("sensor table full (%zu), packet dropped", max_sensors_)));
            }
            return alerts;
        }
        SensorState state;
        state.temp_min_c = reading.temperature_c;
        state.temp_max_c = reading.temperature_c;
        it = sensors_.emplace(reading.sensor_id, state).first;
        alerts.push_back(make_alert(AlertKind::NewSensor, AlertType::Event, reading.sensor_id,
                                    format("first packet, seq %u", static_cast<unsigned>(reading.sequence))));
    } else {
        SensorState& state = it->second;
        check_sequence(state, reading, alerts);
        if (state.stale) {
            state.stale = false;
            const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_seen);
            alerts.push_back(make_alert(AlertKind::Stale, AlertType::Cleared, reading.sensor_id,
                                        format("data resumed after %lld ms", static_cast<long long>(silent.count()))));
        }
    }

    SensorState& state = it->second;
    state.last = reading;
    state.last_seen = now;
    ++state.packets;
    state.temp_min_c = std::min(state.temp_min_c, reading.temperature_c);
    state.temp_max_c = std::max(state.temp_max_c, reading.temperature_c);
    state.temp_sum_c += reading.temperature_c;
    check_thresholds(state, reading, alerts);
    return alerts;
}

void HealthMonitor::check_sequence(SensorState& state, const Reading& reading, std::vector<Alert>& alerts) {
    // Unsigned arithmetic makes the 2^32 wrap-around come out right:
    // last = 0xFFFFFFFF, next = 0 gives gap == 0.
    const std::uint32_t expected = state.last.sequence + 1U;
    const std::uint32_t gap = reading.sequence - expected;
    if (gap == 0) {
        return;
    }
    if (gap < kMaxSequenceGap) {
        state.missed += gap;
        alerts.push_back(make_alert(AlertKind::PacketLoss, AlertType::Event, reading.sensor_id,
                                    format("%u packet(s) missing (expected seq %u, got %u)",
                                           static_cast<unsigned>(gap), static_cast<unsigned>(expected),
                                           static_cast<unsigned>(reading.sequence))));
    } else {
        // Went backwards, repeated, or jumped implausibly far: assume the node
        // restarted its counter.
        ++state.resets;
        alerts.push_back(make_alert(AlertKind::SequenceReset, AlertType::Event, reading.sensor_id,
                                    format("sequence restarted (seq %u after %u)",
                                           static_cast<unsigned>(reading.sequence),
                                           static_cast<unsigned>(state.last.sequence))));
    }
}

void HealthMonitor::check_thresholds(SensorState& state, const Reading& r, std::vector<Alert>& alerts) const {
    const Thresholds& t = thresholds_;
    const std::string& id = r.sensor_id;

    update_condition(state.over_temperature, r.temperature_c > t.temp_high_c,
                     r.temperature_c < t.temp_high_c - t.temp_hysteresis_c, AlertKind::OverTemperature, id,
                     format("%.2f C above limit %.2f C", r.temperature_c, t.temp_high_c),
                     format("%.2f C back below %.2f C", r.temperature_c, t.temp_high_c), alerts);

    update_condition(state.under_temperature, r.temperature_c < t.temp_low_c,
                     r.temperature_c > t.temp_low_c + t.temp_hysteresis_c, AlertKind::UnderTemperature, id,
                     format("%.2f C below limit %.2f C", r.temperature_c, t.temp_low_c),
                     format("%.2f C back above %.2f C", r.temperature_c, t.temp_low_c), alerts);

    update_condition(state.high_humidity, r.humidity_pct > t.humidity_high_pct,
                     r.humidity_pct < t.humidity_high_pct - t.humidity_hysteresis_pct, AlertKind::HighHumidity, id,
                     format("%.1f %% above limit %.1f %%", r.humidity_pct, t.humidity_high_pct),
                     format("%.1f %% back below %.1f %%", r.humidity_pct, t.humidity_high_pct), alerts);

    update_condition(state.low_voltage, r.voltage_v < t.voltage_low_v,
                     r.voltage_v > t.voltage_low_v + t.voltage_hysteresis_v, AlertKind::LowVoltage, id,
                     format("%.2f V below limit %.2f V", r.voltage_v, t.voltage_low_v),
                     format("%.2f V back above %.2f V", r.voltage_v, t.voltage_low_v), alerts);
}

void HealthMonitor::on_parse_error(ParseError error) noexcept {
    const auto index = static_cast<std::size_t>(error);
    if (index < parse_errors_.size()) {
        ++parse_errors_[index];
    }
}

std::vector<Alert> HealthMonitor::check_staleness(Clock::time_point now) {
    std::vector<Alert> alerts;
    for (auto& [id, state] : sensors_) {
        if (state.stale) {
            continue;
        }
        const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_seen);
        if (silent >= thresholds_.stale_after) {
            state.stale = true;
            alerts.push_back(make_alert(AlertKind::Stale, AlertType::Raised, id,
                                        format("no data for %lld ms", static_cast<long long>(silent.count()))));
        }
    }
    return alerts;
}

HealthSnapshot HealthMonitor::snapshot(Clock::time_point now) const {
    HealthSnapshot snap;
    snap.packets_total = packets_total_;
    snap.parse_errors = parse_errors_;
    for (const auto count : parse_errors_) {
        snap.parse_errors_total += count;
    }
    snap.rejected_packets = rejected_packets_;

    bool all_healthy = true;
    snap.sensors.reserve(sensors_.size());
    for (const auto& [id, state] : sensors_) {
        SensorStatus s;
        s.sensor_id = id;
        s.last = state.last;
        s.packets = state.packets;
        s.missed = state.missed;
        s.resets = state.resets;
        s.temp_min_c = state.temp_min_c;
        s.temp_max_c = state.temp_max_c;
        s.temp_mean_c = state.packets > 0 ? state.temp_sum_c / static_cast<double>(state.packets) : 0.0;
        s.age = std::max(std::chrono::milliseconds{0},
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_seen));
        s.stale = state.stale;
        s.over_temperature = state.over_temperature;
        s.under_temperature = state.under_temperature;
        s.high_humidity = state.high_humidity;
        s.low_voltage = state.low_voltage;
        all_healthy = all_healthy && s.healthy();
        snap.sensors.push_back(std::move(s));
    }

    if (snap.sensors.empty()) {
        snap.status = OverallStatus::NoData;
    } else {
        snap.status = all_healthy ? OverallStatus::Ok : OverallStatus::Degraded;
    }
    return snap;
}

}  // namespace telemetry
