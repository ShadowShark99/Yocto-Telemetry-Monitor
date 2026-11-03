// SPDX-License-Identifier: MIT
#include "telemetry/status_json.hpp"

#include <array>
#include <cstdio>

namespace telemetry {
namespace {

std::string number(double value, int decimals) {
    std::array<char, 48> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);
    return buffer.data();
}

}  // namespace

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::array<char, 8> code{};
                    std::snprintf(code.data(), code.size(), "\\u%04x", static_cast<unsigned>(c));
                    out += code.data();
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string to_json(const HealthSnapshot& snapshot, std::chrono::seconds uptime, std::string_view version) {
    std::string j;
    j.reserve(512 + snapshot.sensors.size() * 512);

    j += "{\n";
    j += "  \"version\": \"" + json_escape(version) + "\",\n";
    j += "  \"uptime_s\": " + std::to_string(uptime.count()) + ",\n";
    j += "  \"status\": \"" + std::string(to_string(snapshot.status)) + "\",\n";
    j += "  \"packets_total\": " + std::to_string(snapshot.packets_total) + ",\n";
    j += "  \"parse_errors_total\": " + std::to_string(snapshot.parse_errors_total) + ",\n";
    j += "  \"rejected_packets\": " + std::to_string(snapshot.rejected_packets) + ",\n";

    j += "  \"parse_errors\": {";
    bool first = true;
    for (std::size_t i = 0; i < snapshot.parse_errors.size(); ++i) {
        if (snapshot.parse_errors[i] == 0) {
            continue;
        }
        j += first ? "" : ", ";
        first = false;
        j += "\"" + std::string(to_string(static_cast<ParseError>(i))) + "\": ";
        j += std::to_string(snapshot.parse_errors[i]);
    }
    j += "},\n";

    j += "  \"sensors\": [";
    for (std::size_t i = 0; i < snapshot.sensors.size(); ++i) {
        const SensorStatus& s = snapshot.sensors[i];
        j += i == 0 ? "\n" : ",\n";
        j += "    {\n";
        j += "      \"id\": \"" + json_escape(s.sensor_id) + "\",\n";
        j += "      \"healthy\": " + std::string(s.healthy() ? "true" : "false") + ",\n";
        j += "      \"alarms\": [";
        const auto alarms = s.active_alarms();
        for (std::size_t a = 0; a < alarms.size(); ++a) {
            j += a == 0 ? "\"" : ", \"";
            j += to_string(alarms[a]);
            j += "\"";
        }
        j += "],\n";
        j += "      \"age_ms\": " + std::to_string(s.age.count()) + ",\n";
        j += "      \"sequence\": " + std::to_string(s.last.sequence) + ",\n";
        j += "      \"temperature_c\": " + number(s.last.temperature_c, 2) + ",\n";
        j += "      \"humidity_pct\": " + number(s.last.humidity_pct, 1) + ",\n";
        j += "      \"voltage_v\": " + number(s.last.voltage_v, 2) + ",\n";
        j += "      \"temp_min_c\": " + number(s.temp_min_c, 2) + ",\n";
        j += "      \"temp_max_c\": " + number(s.temp_max_c, 2) + ",\n";
        j += "      \"temp_mean_c\": " + number(s.temp_mean_c, 2) + ",\n";
        j += "      \"packets\": " + std::to_string(s.packets) + ",\n";
        j += "      \"missed\": " + std::to_string(s.missed) + ",\n";
        j += "      \"resets\": " + std::to_string(s.resets) + "\n";
        j += "    }";
    }
    j += snapshot.sensors.empty() ? "]\n" : "\n  ]\n";
    j += "}\n";
    return j;
}

}  // namespace telemetry
