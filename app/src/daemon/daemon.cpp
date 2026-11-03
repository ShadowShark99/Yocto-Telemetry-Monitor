// SPDX-License-Identifier: MIT
#include "daemon.hpp"

#include <poll.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <variant>

#include "platform/log.hpp"
#include "platform/sd_notify.hpp"
#include "telemetry/packet.hpp"
#include "telemetry/status_json.hpp"
#include "telemetry/version.hpp"

namespace telemetry {
namespace {

constexpr int kSerialRetryTicks = 5;

std::string describe_inputs(const Config& config) {
    std::string inputs;
    if (config.udp_port != 0) {
        inputs += "udp " + config.udp_bind_address + ":" + std::to_string(config.udp_port);
    }
    if (!config.serial_device.empty()) {
        inputs += inputs.empty() ? "" : ", ";
        inputs += "serial " + config.serial_device + " @ " + std::to_string(config.serial_baud);
    }
    return inputs;
}

std::string alarm_list(const SensorStatus& sensor) {
    std::string text;
    for (const auto kind : sensor.active_alarms()) {
        text += text.empty() ? " ALARMS:" : ",";
        text += ' ';
        text += to_string(kind);
    }
    return text;
}

}  // namespace

void CliOverrides::apply(Config& config) const {
    if (udp_bind_address) config.udp_bind_address = *udp_bind_address;
    if (udp_port) config.udp_port = *udp_port;
    if (serial_device) config.serial_device = *serial_device;
    if (serial_baud) config.serial_baud = *serial_baud;
    if (status_file) config.status_file = *status_file;
    if (log_level) config.log_level = *log_level;
}

ConfigResult build_config(const std::string& config_path, const CliOverrides& overrides) {
    ConfigResult result = config_path.empty() ? ConfigResult{} : load_config_file(config_path);
    overrides.apply(result.config);
    for (auto& message : validate(result.config)) {
        result.errors.push_back(std::move(message));
    }
    return result;
}

Daemon::Daemon(std::string config_path, CliOverrides overrides, Config initial)
    : config_path_(std::move(config_path)),
      overrides_(std::move(overrides)),
      config_(std::move(initial)),
      monitor_(config_.thresholds, config_.max_sensors) {}

int Daemon::run() {
    started_ = Clock::now();
    last_report_ = started_;
    log::set_level(config_.log_level);

    if (!setup_signals() || !setup_timer()) {
        return EXIT_FAILURE;
    }
    if (config_.udp_port != 0 && !open_udp(config_, udp_)) {
        return EXIT_FAILURE;  // systemd's Restart= will retry
    }
    if (!config_.serial_device.empty()) {
        serial_.emplace(config_.serial_device, config_.serial_baud);
        open_serial_if_needed(true);
    }

    const auto watchdog = sd::watchdog_interval();
    watchdog_enabled_ = watchdog.has_value();

    TLOG_INFO("telemetryd %s started: %s%s", kVersion, describe_inputs(config_).c_str(),
              watchdog_enabled_ ? ", systemd watchdog on" : "");
    sd::notify("READY=1\nSTATUS=Waiting for telemetry");

    std::array<pollfd, 4> fds{};
    while (running_) {
        nfds_t count = 0;
        fds[count++] = pollfd{signal_fd_.get(), POLLIN, 0};
        fds[count++] = pollfd{timer_fd_.get(), POLLIN, 0};
        const nfds_t udp_index = udp_ ? count++ : 0;
        if (udp_) {
            fds[udp_index] = pollfd{udp_->fd(), POLLIN, 0};
        }
        const bool serial_open = serial_ && serial_->is_open();
        const nfds_t serial_index = serial_open ? count++ : 0;
        if (serial_open) {
            fds[serial_index] = pollfd{serial_->fd(), POLLIN, 0};
        }

        if (::poll(fds.data(), count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            TLOG_ERROR("poll: %s", std::strerror(errno));
            return EXIT_FAILURE;
        }

        if (fds[0].revents & POLLIN) {
            on_signal();
        }
        if (fds[1].revents & POLLIN) {
            on_tick();
        }
        if (udp_ && udp_index != 0 && (fds[udp_index].revents & POLLIN)) {
            on_udp();
        }
        if (serial_open && serial_index != 0 && fds[serial_index].revents != 0) {
            on_serial(fds[serial_index].revents);
        }
    }

    sd::notify("STOPPING=1\nSTATUS=Shutting down");
    write_status_file(Clock::now());
    TLOG_INFO("shutting down after %llu packets",
              static_cast<unsigned long long>(monitor_.snapshot(Clock::now()).packets_total));
    return EXIT_SUCCESS;
}

bool Daemon::setup_signals() {
    // Block the signals so they are only delivered through the signalfd.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        TLOG_ERROR("sigprocmask: %s", std::strerror(errno));
        return false;
    }
    signal_fd_.reset(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!signal_fd_) {
        TLOG_ERROR("signalfd: %s", std::strerror(errno));
        return false;
    }
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}

bool Daemon::setup_timer() {
    timer_fd_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    if (!timer_fd_) {
        TLOG_ERROR("timerfd_create: %s", std::strerror(errno));
        return false;
    }
    itimerspec period{};
    period.it_value.tv_sec = 1;
    period.it_interval.tv_sec = 1;
    if (::timerfd_settime(timer_fd_.get(), 0, &period, nullptr) != 0) {
        TLOG_ERROR("timerfd_settime: %s", std::strerror(errno));
        return false;
    }
    return true;
}

bool Daemon::open_udp(const Config& config, std::optional<UdpSource>& out) {
    std::string error;
    out = UdpSource::open(config.udp_bind_address, config.udp_port, error);
    if (!out) {
        TLOG_ERROR("cannot listen for UDP telemetry: %s", error.c_str());
        return false;
    }
    return true;
}

void Daemon::open_serial_if_needed(bool force_log) {
    if (!serial_ || serial_->is_open()) {
        return;
    }
    std::string error;
    if (serial_->open(error)) {
        TLOG_INFO("serial input %s opened at %u baud", serial_->device().c_str(),
                  static_cast<unsigned>(serial_->baud()));
        serial_failure_logged_ = false;
        return;
    }
    // Warn once, then keep retrying quietly until the device appears.
    if (force_log || !serial_failure_logged_) {
        TLOG_WARN("serial input unavailable (%s); retrying every %d s", error.c_str(), kSerialRetryTicks);
        serial_failure_logged_ = true;
    } else {
        TLOG_DEBUG("serial retry failed: %s", error.c_str());
    }
}

void Daemon::on_signal() {
    signalfd_siginfo info{};
    while (::read(signal_fd_.get(), &info, sizeof info) == static_cast<ssize_t>(sizeof info)) {
        switch (info.ssi_signo) {
            case SIGHUP:
                reload();
                break;
            case SIGINT:
            case SIGTERM:
                TLOG_INFO("received %s", strsignal(static_cast<int>(info.ssi_signo)));
                running_ = false;
                break;
            default:
                break;
        }
    }
}

void Daemon::on_tick() {
    std::uint64_t expirations = 0;
    if (::read(timer_fd_.get(), &expirations, sizeof expirations) != static_cast<ssize_t>(sizeof expirations)) {
        return;
    }
    ++ticks_;
    const auto now = Clock::now();

    on_alerts(monitor_.check_staleness(now));

    if (serial_ && !serial_->is_open() && ticks_ % kSerialRetryTicks == 0) {
        open_serial_if_needed(false);
    }

    write_status_file(now);

    const HealthSnapshot snapshot = monitor_.snapshot(now);
    std::string state = "STATUS=" + status_line(snapshot);
    if (watchdog_enabled_) {
        state = "WATCHDOG=1\n" + state;
    }
    sd::notify(state);

    if (now - last_report_ >= config_.report_interval) {
        report(now);
    }
}

void Daemon::on_udp() {
    udp_->drain([this](std::string_view line) { on_line(line, "udp"); });
}

void Daemon::on_serial(short revents) {
    std::string error;
    bool healthy = true;
    // Drain whatever arrived before acting on a hang-up, so the last bytes
    // sent before a disconnect are not lost.
    if (revents & POLLIN) {
        healthy = serial_->read_lines([this](std::string_view line) { on_line(line, "serial"); }, error);
    }
    if (healthy && (revents & (POLLERR | POLLHUP | POLLNVAL))) {
        healthy = false;
        error = serial_->device() + ((revents & POLLHUP) ? ": hung up" : ": device error");
    }
    if (!healthy) {
        TLOG_WARN("serial input lost (%s); will retry", error.c_str());
        serial_->close();
        serial_failure_logged_ = true;
    }
}

void Daemon::on_line(std::string_view line, const char* source) {
    const auto now = Clock::now();
    const ParseResult result = parse_packet(line);
    if (const auto* error = std::get_if<ParseError>(&result)) {
        monitor_.on_parse_error(*error);
        if (log::enabled(LogLevel::Debug)) {
            const std::string text(line.substr(0, 80));
            TLOG_DEBUG("rejected %s line (%s): %s", source, to_string(*error), text.c_str());
        }
        return;
    }
    const Reading& reading = std::get<Reading>(result);
    TLOG_DEBUG("%s %s seq=%u T=%.2f RH=%.1f V=%.2f", source, reading.sensor_id.c_str(),
               static_cast<unsigned>(reading.sequence), reading.temperature_c, reading.humidity_pct,
               reading.voltage_v);
    on_alerts(monitor_.on_reading(reading, now));
}

void Daemon::on_alerts(const std::vector<Alert>& alerts) {
    for (const Alert& alert : alerts) {
        const char* kind = to_string(alert.kind);
        const char* id = alert.sensor_id.c_str();
        const char* message = alert.message.c_str();
        switch (alert.type) {
            case AlertType::Raised:
                TLOG_WARN("ALARM %s %s: %s", id, kind, message);
                break;
            case AlertType::Cleared:
                TLOG_INFO("CLEAR %s %s: %s", id, kind, message);
                break;
            case AlertType::Event:
                if (alert.kind == AlertKind::PacketLoss || alert.kind == AlertKind::SensorRejected) {
                    TLOG_WARN("%s %s: %s", id, kind, message);
                } else {
                    TLOG_INFO("%s %s: %s", id, kind, message);
                }
                break;
        }
    }
}

void Daemon::reload() {
    sd::notify("RELOADING=1\nMONOTONIC_USEC=" + std::to_string(sd::monotonic_usec()));
    TLOG_INFO("reloading configuration%s%s", config_path_.empty() ? "" : " from ", config_path_.c_str());

    ConfigResult result = build_config(config_path_, overrides_);
    if (!result.ok()) {
        for (const auto& error : result.errors) {
            TLOG_ERROR("%s", error.c_str());
        }
        TLOG_ERROR("reload rejected; keeping the running configuration");
        sd::notify("READY=1");
        return;
    }
    Config next = std::move(result.config);

    // Inputs: open the new socket before dropping the old one, so a bad
    // address does not leave the daemon deaf.
    if (next.udp_port != config_.udp_port || next.udp_bind_address != config_.udp_bind_address) {
        if (next.udp_port == 0) {
            udp_.reset();
            TLOG_INFO("UDP input disabled");
        } else {
            std::optional<UdpSource> replacement;
            if (open_udp(next, replacement)) {
                udp_ = std::move(replacement);
                TLOG_INFO("UDP input now on %s:%u", next.udp_bind_address.c_str(), static_cast<unsigned>(next.udp_port));
            } else {
                next.udp_bind_address = config_.udp_bind_address;
                next.udp_port = config_.udp_port;
            }
        }
    }
    if (next.serial_device != config_.serial_device || next.serial_baud != config_.serial_baud) {
        serial_.reset();
        if (!next.serial_device.empty()) {
            serial_.emplace(next.serial_device, next.serial_baud);
            open_serial_if_needed(true);
        } else {
            TLOG_INFO("serial input disabled");
        }
    }

    monitor_.set_thresholds(next.thresholds);
    monitor_.set_max_sensors(next.max_sensors);
    log::set_level(next.log_level);
    config_ = std::move(next);
    status_write_failed_ = false;

    TLOG_INFO("configuration reloaded: %s", describe_inputs(config_).c_str());
    sd::notify("READY=1");
}

void Daemon::report(Clock::time_point now) {
    const HealthSnapshot snapshot = monitor_.snapshot(now);
    const double seconds = std::chrono::duration<double>(now - last_report_).count();
    const auto delta = snapshot.packets_total - packets_at_last_report_;
    last_report_ = now;
    packets_at_last_report_ = snapshot.packets_total;

    TLOG_INFO("report: status=%s sensors=%zu packets=%llu (+%llu, %.1f/s) parse_errors=%llu",
              to_string(snapshot.status), snapshot.sensors.size(),
              static_cast<unsigned long long>(snapshot.packets_total), static_cast<unsigned long long>(delta),
              seconds > 0.0 ? static_cast<double>(delta) / seconds : 0.0,
              static_cast<unsigned long long>(snapshot.parse_errors_total));
    for (const SensorStatus& s : snapshot.sensors) {
        TLOG_INFO("  %-12s T=%6.2f C  RH=%5.1f %%  V=%4.2f V  pkts=%llu missed=%llu age=%lldms%s",
                  s.sensor_id.c_str(), s.last.temperature_c, s.last.humidity_pct, s.last.voltage_v,
                  static_cast<unsigned long long>(s.packets), static_cast<unsigned long long>(s.missed),
                  static_cast<long long>(s.age.count()), alarm_list(s).c_str());
    }
}

void Daemon::write_status_file(Clock::time_point now) {
    if (config_.status_file.empty()) {
        return;
    }
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_);
    const std::string json = to_json(monitor_.snapshot(now), uptime, kVersion);

    // Write to a temporary file and rename() it over the old one: readers
    // always see either the previous or the new document, never half of one.
    const std::string temporary = config_.status_file + ".tmp";
    bool ok = false;
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (out) {
            out << json;
            out.close();
            ok = static_cast<bool>(out);
        }
    }
    if (ok && std::rename(temporary.c_str(), config_.status_file.c_str()) != 0) {
        ok = false;
    }
    if (!ok && !status_write_failed_) {
        TLOG_WARN("cannot write status file %s: %s (further failures not logged)", config_.status_file.c_str(),
                  std::strerror(errno));
        status_write_failed_ = true;
    } else if (ok && status_write_failed_) {
        TLOG_INFO("status file %s writable again", config_.status_file.c_str());
        status_write_failed_ = false;
    }
}

std::string Daemon::status_line(const HealthSnapshot& snapshot) const {
    std::size_t alarming = 0;
    for (const auto& sensor : snapshot.sensors) {
        alarming += sensor.healthy() ? 0 : 1;
    }
    std::array<char, 160> text{};
    std::snprintf(text.data(), text.size(), "%s: %zu sensor(s), %zu alarming, %llu packets, %llu rejected",
                  to_string(snapshot.status), snapshot.sensors.size(), alarming,
                  static_cast<unsigned long long>(snapshot.packets_total),
                  static_cast<unsigned long long>(snapshot.parse_errors_total));
    return text.data();
}

}  // namespace telemetry
