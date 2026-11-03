// SPDX-License-Identifier: MIT
//
// telemetry-sim: generates realistic sensor packets for telemetryd.
//
// Each simulated node reports temperature (slow sine plus noise), humidity
// and a battery voltage that drains and recharges. Optional faults exercise
// the daemon's error paths: corrupted checksums, dropped packets (sequence
// gaps) and periodic over-temperature spikes.
//
// Packets go to a UDP address by default, or to a file / tty / stdout with
// --output, so the same tool can drive a real serial link.
#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "platform/serial_port.hpp"
#include "platform/unique_fd.hpp"
#include "telemetry/packet.hpp"
#include "telemetry/version.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_stop_signal(int /*signal*/) {
    g_stop = 1;
}

constexpr double kPi = 3.14159265358979323846;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5500;
    std::string output;  // file, tty or "-" instead of UDP
    std::uint32_t baud = 115200;
    unsigned sensors = 3;
    std::string id_prefix = "env";
    double rate_hz = 1.0;
    std::uint64_t count = 0;  // packets per sensor, 0 = run until stopped
    double fault_rate = 0.0;
    double drop_rate = 0.0;
    double spike_every_s = 0.0;
    std::uint32_t seed = 0;
    bool seed_set = false;
    bool quiet = false;
};

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "Usage: telemetry-sim [options]\n"
                 "\n"
                 "  -H, --host ADDRESS     UDP destination (default 127.0.0.1)\n"
                 "  -p, --port PORT        UDP port (default 5500)\n"
                 "  -o, --output PATH      write lines to a file, tty or '-' (stdout) instead of UDP\n"
                 "  -B, --baud RATE        baud rate when --output is a tty (default 115200)\n"
                 "  -n, --sensors N        number of simulated nodes, 1-99 (default 3)\n"
                 "  -i, --id-prefix TEXT   sensor IDs are TEXT-01, TEXT-02, ... (default env)\n"
                 "  -r, --rate HZ          packets per second per node (default 1)\n"
                 "  -c, --count N          packets per node, then exit (default: run forever)\n"
                 "  -f, --fault-rate P     probability of a corrupted checksum (default 0)\n"
                 "  -d, --drop-rate P      probability of skipping a packet (default 0)\n"
                 "  -k, --spike-every S    push one node over 80 C for 8 s every S seconds\n"
                 "  -s, --seed N           random seed, for reproducible runs\n"
                 "  -q, --quiet            only print the final summary\n"
                 "  -V, --version          print the version and exit\n"
                 "  -h, --help             show this help\n");
}

bool parse_double_arg(const char* text, double& out, double low, double high) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(value) || value < low || value > high) {
        return false;
    }
    out = value;
    return true;
}

template <typename Int>
bool parse_uint_arg(const char* text, Int& out, unsigned long long low, unsigned long long high) {
    unsigned long long value = 0;
    const char* end = text + std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, end, value);
    if (ec != std::errc{} || ptr != end || value < low || value > high) {
        return false;
    }
    out = static_cast<Int>(value);
    return true;
}

bool parse_args(int argc, char** argv, Options& o, int& exit_code) {
    static const option long_options[] = {
        {"host", required_argument, nullptr, 'H'},        {"port", required_argument, nullptr, 'p'},
        {"output", required_argument, nullptr, 'o'},      {"baud", required_argument, nullptr, 'B'},
        {"sensors", required_argument, nullptr, 'n'},     {"id-prefix", required_argument, nullptr, 'i'},
        {"rate", required_argument, nullptr, 'r'},
        {"count", required_argument, nullptr, 'c'},       {"fault-rate", required_argument, nullptr, 'f'},
        {"drop-rate", required_argument, nullptr, 'd'},   {"spike-every", required_argument, nullptr, 'k'},
        {"seed", required_argument, nullptr, 's'},        {"quiet", no_argument, nullptr, 'q'},
        {"version", no_argument, nullptr, 'V'},           {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "H:p:o:B:n:i:r:c:f:d:k:s:qVh", long_options, nullptr)) != -1) {
        bool ok = true;
        switch (opt) {
            case 'H': o.host = optarg; break;
            case 'p': ok = parse_uint_arg(optarg, o.port, 1, 65535); break;
            case 'o': o.output = optarg; break;
            case 'B': ok = parse_uint_arg(optarg, o.baud, 1, 4000000); break;
            case 'n': ok = parse_uint_arg(optarg, o.sensors, 1, 99); break;
            case 'i':
                o.id_prefix = optarg;
                // "<prefix>-NN" must still be a valid sensor ID.
                ok = o.id_prefix.size() <= telemetry::kMaxSensorIdLength - 3 &&
                     telemetry::is_valid_sensor_id(o.id_prefix);
                break;
            case 'r': ok = parse_double_arg(optarg, o.rate_hz, 0.01, 1000.0); break;
            case 'c': ok = parse_uint_arg(optarg, o.count, 0, 1ULL << 62); break;
            case 'f': ok = parse_double_arg(optarg, o.fault_rate, 0.0, 1.0); break;
            case 'd': ok = parse_double_arg(optarg, o.drop_rate, 0.0, 1.0); break;
            case 'k': ok = parse_double_arg(optarg, o.spike_every_s, 0.0, 86400.0); break;
            case 's':
                ok = parse_uint_arg(optarg, o.seed, 0, 0xFFFFFFFFULL);
                o.seed_set = true;
                break;
            case 'q': o.quiet = true; break;
            case 'V':
                std::printf("telemetry-sim %s\n", telemetry::kVersion);
                exit_code = EXIT_SUCCESS;
                return false;
            case 'h':
                print_usage(stdout);
                exit_code = EXIT_SUCCESS;
                return false;
            default:
                print_usage(stderr);
                exit_code = 64;
                return false;
        }
        if (!ok) {
            std::fprintf(stderr, "telemetry-sim: invalid value '%s' for -%c\n", optarg, opt);
            exit_code = 64;
            return false;
        }
    }
    if (o.spike_every_s > 0.0 && o.spike_every_s < 10.0) {
        std::fprintf(stderr, "telemetry-sim: --spike-every must be 0 or at least 10 seconds\n");
        exit_code = 64;
        return false;
    }
    return true;
}

// Where packets go: a connected UDP socket or a byte stream.
class Sink {
public:
    bool open(const Options& o) {
        if (o.output.empty()) {
            return open_udp(o);
        }
        if (o.output == "-") {
            fd_.reset(::dup(STDOUT_FILENO));
            stream_ = true;
            return static_cast<bool>(fd_);
        }
        fd_.reset(::open(o.output.c_str(), O_WRONLY | O_NOCTTY | O_CREAT | O_APPEND | O_CLOEXEC, 0644));
        if (!fd_) {
            std::fprintf(stderr, "telemetry-sim: %s: %s\n", o.output.c_str(), std::strerror(errno));
            return false;
        }
        if (::isatty(fd_.get())) {
            std::string error;
            if (!telemetry::configure_serial(fd_.get(), o.baud, error)) {
                std::fprintf(stderr, "telemetry-sim: %s: %s\n", o.output.c_str(), error.c_str());
                return false;
            }
        }
        stream_ = true;
        return true;
    }

    // Returns false only on errors worth stopping for.
    bool send(const std::string& line) {
        const std::string data = line + "\n";
        if (!stream_) {
            const ssize_t n = ::send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL);
            // ECONNREFUSED: nobody listening yet (ICMP port unreachable). Keep going.
            if (n < 0 && errno != ECONNREFUSED && errno != EINTR) {
                ++send_errors_;
            }
            return true;
        }
        std::size_t written = 0;
        while (written < data.size()) {
            const ssize_t n = ::write(fd_.get(), data.data() + written, data.size() - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::fprintf(stderr, "telemetry-sim: write: %s\n", std::strerror(errno));
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::uint64_t send_errors() const noexcept { return send_errors_; }

private:
    bool open_udp(const Options& o) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(o.port);
        if (inet_pton(AF_INET, o.host.c_str(), &address.sin_addr) != 1) {
            std::fprintf(stderr, "telemetry-sim: invalid IPv4 address '%s'\n", o.host.c_str());
            return false;
        }
        fd_.reset(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        if (!fd_ || ::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
            std::fprintf(stderr, "telemetry-sim: socket: %s\n", std::strerror(errno));
            return false;
        }
        return true;
    }

    telemetry::UniqueFd fd_;
    bool stream_ = false;
    std::uint64_t send_errors_ = 0;
};

struct Node {
    std::string id;
    std::uint32_t sequence = 0;
    double base_temp_c = 22.0;
    double phase = 0.0;
    double battery_offset_s = 0.0;
};

timespec add_ns(timespec t, long long ns) {
    const long long total = static_cast<long long>(t.tv_nsec) + ns;
    t.tv_sec += static_cast<time_t>(total / 1000000000LL);
    t.tv_nsec = static_cast<long>(total % 1000000000LL);
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    int exit_code = EXIT_SUCCESS;
    if (!parse_args(argc, argv, o, exit_code)) {
        return exit_code;
    }

    struct sigaction action {};
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    Sink sink;
    if (!sink.open(o)) {
        return EXIT_FAILURE;
    }

    std::mt19937 rng(o.seed_set ? o.seed : std::random_device{}());
    std::normal_distribution<double> noise(0.0, 0.15);
    std::uniform_real_distribution<double> chance(0.0, 1.0);

    std::vector<Node> nodes(o.sensors);
    for (unsigned i = 0; i < o.sensors; ++i) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, "-%02u", i + 1);
        nodes[i].id = o.id_prefix + suffix;
        nodes[i].base_temp_c = 21.0 + 2.5 * i;
        nodes[i].phase = 1.7 * i;
        nodes[i].battery_offset_s = 200.0 * i;  // nodes reach low battery at different times
    }

    if (!o.quiet) {
        std::fprintf(stderr, "telemetry-sim: %u node(s) at %.2f Hz -> %s\n", o.sensors, o.rate_hz,
                     o.output.empty() ? (o.host + ":" + std::to_string(o.port)).c_str() : o.output.c_str());
    }

    const long long period_ns = static_cast<long long>(1e9 / o.rate_hz);
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    const timespec start = next;

    std::uint64_t sent = 0;
    std::uint64_t corrupted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t round = 0;

    while (!g_stop && (o.count == 0 || round < o.count)) {
        const double t = static_cast<double>(next.tv_sec - start.tv_sec) +
                         static_cast<double>(next.tv_nsec - start.tv_nsec) / 1e9;

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            Node& node = nodes[i];
            telemetry::Reading r;
            r.sensor_id = node.id;
            r.sequence = node.sequence++;
            r.temperature_c = node.base_temp_c + 2.5 * std::sin(2.0 * kPi * t / 120.0 + node.phase) + noise(rng);
            r.humidity_pct = std::clamp(45.0 + 10.0 * std::sin(2.0 * kPi * t / 300.0 + node.phase) + noise(rng),
                                        0.0, 100.0);
            // Battery: 3.30 V draining to 2.90 V over 10 minutes, then "recharged".
            r.voltage_v = 3.30 - 0.40 * std::fmod(t + node.battery_offset_s, 600.0) / 600.0 + noise(rng) * 0.02;

            if (o.spike_every_s > 0.0) {
                const auto window = static_cast<std::uint64_t>(t / o.spike_every_s);
                const double into_window = t - static_cast<double>(window) * o.spike_every_s;
                if (window > 0 && window % nodes.size() == i && into_window < 8.0) {
                    r.temperature_c += 60.0;  // fan failure, direct sunlight, ...
                }
            }

            if (chance(rng) < o.drop_rate) {
                ++dropped;  // sequence number consumed, packet never sent
                continue;
            }
            std::string line = telemetry::format_packet(r);
            if (chance(rng) < o.fault_rate) {
                line.back() = line.back() == '0' ? '1' : '0';  // flip the checksum
                ++corrupted;
            }
            if (!sink.send(line)) {
                return EXIT_FAILURE;
            }
            ++sent;
            if (!o.quiet && o.output != "-") {
                std::printf("%s\n", line.c_str());
                std::fflush(stdout);
            }
        }
        ++round;

        next = add_ns(next, period_ns);
        while (!g_stop && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr) == EINTR) {
        }
    }

    // Machine-readable summary; the integration test parses it.
    std::fprintf(o.output == "-" ? stderr : stdout,
                 "sent=%llu corrupted=%llu dropped=%llu send_errors=%llu\n", static_cast<unsigned long long>(sent),
                 static_cast<unsigned long long>(corrupted), static_cast<unsigned long long>(dropped),
                 static_cast<unsigned long long>(sink.send_errors()));
    return EXIT_SUCCESS;
}
