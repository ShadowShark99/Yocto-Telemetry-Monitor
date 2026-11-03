// SPDX-License-Identifier: MIT
//
// Renders a HealthSnapshot as the JSON document the daemon publishes at
// /run/telemetryd/status.json (and that the on-target tests read).
#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "telemetry/health_monitor.hpp"

namespace telemetry {

std::string to_json(const HealthSnapshot& snapshot, std::chrono::seconds uptime, std::string_view version);

/// Escapes a string for inclusion inside JSON double quotes.
std::string json_escape(std::string_view text);

}  // namespace telemetry
