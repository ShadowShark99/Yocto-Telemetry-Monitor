// SPDX-License-Identifier: MIT
//
// telemetryd: receives sensor telemetry over UDP and/or a serial port,
// validates it and monitors sensor health. Designed to run as a systemd
// Type=notify service; see meta-telemetry/.../telemetryd.service.
#include <getopt.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "daemon.hpp"
#include "platform/log.hpp"
#include "telemetry/version.hpp"

namespace {

// sysexits.h EX_USAGE / EX_CONFIG; systemd shows the latter as "78/CONFIG".
constexpr int kExitUsage = 64;
constexpr int kExitConfig = 78;

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "Usage: telemetryd [options]\n"
                 "\n"
                 "Receive and monitor sensor telemetry.\n"
                 "\n"
                 "  -c, --config PATH       configuration file (default: built-in defaults only)\n"
                 "  -b, --bind ADDRESS      IPv4 address for UDP input (default 0.0.0.0)\n"
                 "  -p, --udp-port PORT     UDP port, 0 disables UDP input (default 5500)\n"
                 "  -s, --serial DEVICE     serial device to read, e.g. /dev/ttyUSB0\n"
                 "  -B, --baud RATE         serial baud rate (default 115200)\n"
                 "  -S, --status-file PATH  JSON status file, \"\" disables\n"
                 "                          (default /run/telemetryd/status.json)\n"
                 "  -l, --log-level LEVEL   debug, info, warning or error\n"
                 "  -t, --check-config      validate the configuration and exit\n"
                 "  -V, --version           print the version and exit\n"
                 "  -h, --help              show this help\n"
                 "\n"
                 "Command-line options override the configuration file.\n"
                 "SIGHUP reloads the configuration; SIGTERM stops the daemon.\n");
}

template <typename Int>
bool parse_uint(const char* text, Int& out, unsigned long long max) {
    unsigned long long value = 0;
    const char* end = text + std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, end, value);
    if (ec != std::errc{} || ptr != end || value > max) {
        return false;
    }
    out = static_cast<Int>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace telemetry;

    log::init();

    std::string config_path;
    CliOverrides overrides;
    bool check_only = false;

    static const option long_options[] = {
        {"config", required_argument, nullptr, 'c'},      {"bind", required_argument, nullptr, 'b'},
        {"udp-port", required_argument, nullptr, 'p'},    {"serial", required_argument, nullptr, 's'},
        {"baud", required_argument, nullptr, 'B'},        {"status-file", required_argument, nullptr, 'S'},
        {"log-level", required_argument, nullptr, 'l'},   {"check-config", no_argument, nullptr, 't'},
        {"version", no_argument, nullptr, 'V'},           {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "c:b:p:s:B:S:l:tVh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'b':
                overrides.udp_bind_address = optarg;
                break;
            case 'p': {
                std::uint16_t port = 0;
                if (!parse_uint(optarg, port, 65535)) {
                    std::fprintf(stderr, "telemetryd: invalid UDP port '%s'\n", optarg);
                    return kExitUsage;
                }
                overrides.udp_port = port;
                break;
            }
            case 's':
                overrides.serial_device = optarg;
                break;
            case 'B': {
                std::uint32_t baud = 0;
                if (!parse_uint(optarg, baud, 0xFFFFFFFFULL) || !is_supported_baud(baud)) {
                    std::fprintf(stderr, "telemetryd: unsupported baud rate '%s'\n", optarg);
                    return kExitUsage;
                }
                overrides.serial_baud = baud;
                break;
            }
            case 'S':
                overrides.status_file = optarg;
                break;
            case 'l': {
                const auto level = parse_log_level(optarg);
                if (!level) {
                    std::fprintf(stderr, "telemetryd: invalid log level '%s'\n", optarg);
                    return kExitUsage;
                }
                overrides.log_level = *level;
                break;
            }
            case 't':
                check_only = true;
                break;
            case 'V':
                std::printf("telemetryd %s\n", kVersion);
                return EXIT_SUCCESS;
            case 'h':
                print_usage(stdout);
                return EXIT_SUCCESS;
            default:
                print_usage(stderr);
                return kExitUsage;
        }
    }
    if (optind < argc) {
        std::fprintf(stderr, "telemetryd: unexpected argument '%s'\n", argv[optind]);
        return kExitUsage;
    }

    ConfigResult result = build_config(config_path, overrides);
    if (!result.ok()) {
        for (const auto& error : result.errors) {
            TLOG_ERROR("config: %s", error.c_str());
        }
        return kExitConfig;
    }
    if (check_only) {
        std::printf("configuration OK%s%s\n", config_path.empty() ? "" : ": ", config_path.c_str());
        return EXIT_SUCCESS;
    }

    Daemon daemon(config_path, overrides, std::move(result.config));
    return daemon.run();
}
