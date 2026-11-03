// SPDX-License-Identifier: MIT
//
// The telemetryd event loop.
//
// Everything runs on one thread around poll(2):
//
//   signalfd  SIGTERM/SIGINT -> clean shutdown, SIGHUP -> reload config
//   timerfd   1 s tick       -> staleness checks, status file, watchdog
//   UDP       datagrams      -> parse -> HealthMonitor
//   serial    byte stream    -> LineBuffer -> parse -> HealthMonitor
//
// Handling signals through a file descriptor (instead of async signal
// handlers) means no code runs in signal context and nothing needs locking.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/serial_source.hpp"
#include "platform/udp_source.hpp"
#include "platform/unique_fd.hpp"
#include "telemetry/config.hpp"
#include "telemetry/health_monitor.hpp"

namespace telemetry {

/// Settings given on the command line; they win over the config file and are
/// re-applied on every reload.
struct CliOverrides {
    std::optional<std::string> udp_bind_address;
    std::optional<std::uint16_t> udp_port;
    std::optional<std::string> serial_device;
    std::optional<std::uint32_t> serial_baud;
    std::optional<std::string> status_file;
    std::optional<LogLevel> log_level;

    void apply(Config& config) const;
};

/// Loads `config_path` (if not empty), applies overrides and validates.
ConfigResult build_config(const std::string& config_path, const CliOverrides& overrides);

class Daemon {
public:
    Daemon(std::string config_path, CliOverrides overrides, Config initial);

    /// Runs until SIGTERM/SIGINT. Returns the process exit code.
    int run();

private:
    bool setup_signals();
    bool setup_timer();
    bool open_udp(const Config& config, std::optional<UdpSource>& out);
    void open_serial_if_needed(bool force_log);

    void on_signal();
    void on_tick();
    void on_udp();
    void on_serial(short revents);
    void on_line(std::string_view line, const char* source);
    void on_alerts(const std::vector<Alert>& alerts);

    void reload();
    void report(Clock::time_point now);
    void write_status_file(Clock::time_point now);
    std::string status_line(const HealthSnapshot& snapshot) const;

    std::string config_path_;
    CliOverrides overrides_;
    Config config_;
    HealthMonitor monitor_;

    UniqueFd signal_fd_;
    UniqueFd timer_fd_;
    std::optional<UdpSource> udp_;
    std::optional<SerialSource> serial_;

    Clock::time_point started_;
    Clock::time_point last_report_;
    std::uint64_t packets_at_last_report_ = 0;
    std::uint64_t ticks_ = 0;
    bool running_ = true;
    bool watchdog_enabled_ = false;
    bool status_write_failed_ = false;
    bool serial_failure_logged_ = false;
};

}  // namespace telemetry
