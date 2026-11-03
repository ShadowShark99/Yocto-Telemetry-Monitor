// SPDX-License-Identifier: MIT
//
// The systemd notification protocol (sd_notify(3)) without libsystemd: a
// datagram of "KEY=value" lines sent to the AF_UNIX socket named in
// $NOTIFY_SOCKET. Used for Type=notify readiness, STATUS= text shown by
// `systemctl status`, and WatchdogSec= keep-alives.
#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace telemetry::sd {

/// Sends `state` (e.g. "READY=1"). Returns false when not running under
/// systemd or when the send fails; callers can ignore the result.
bool notify(std::string_view state);

/// The watchdog interval systemd expects keep-alives within, if
/// WatchdogSec= is set for this process.
std::optional<std::chrono::microseconds> watchdog_interval();

/// CLOCK_MONOTONIC in microseconds, for "MONOTONIC_USEC=" on reload.
long long monotonic_usec();

}  // namespace telemetry::sd
