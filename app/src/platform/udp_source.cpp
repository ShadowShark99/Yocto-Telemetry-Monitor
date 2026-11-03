// SPDX-License-Identifier: MIT
#include "udp_source.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace telemetry {
namespace {

constexpr std::size_t kMaxDatagram = 2048;
constexpr std::size_t kMaxDatagramsPerDrain = 64;

}  // namespace

std::optional<UdpSource> UdpSource::open(const std::string& bind_address, std::uint16_t port, std::string& error) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        error = "invalid IPv4 bind address '" + bind_address + "'";
        return std::nullopt;
    }

    UniqueFd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) {
        error = std::string("socket: ") + std::strerror(errno);
        return std::nullopt;
    }
    const int on = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        error = "bind " + bind_address + ":" + std::to_string(port) + ": " + std::strerror(errno);
        return std::nullopt;
    }
    return UdpSource(std::move(fd), bind_address, port);
}

std::size_t UdpSource::drain(const LineHandler& on_line) {
    std::array<char, kMaxDatagram> buffer{};
    std::size_t datagrams = 0;
    while (datagrams < kMaxDatagramsPerDrain) {
        // MSG_TRUNC makes recv() report the real datagram length, so an
        // oversized datagram is detected instead of silently cut.
        const ssize_t received = ::recv(fd_.get(), buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;  // EAGAIN: queue is empty; anything else is transient for UDP
        }
        ++datagrams;
        if (static_cast<std::size_t>(received) > buffer.size()) {
            ++oversized_;
            continue;
        }

        std::string_view data(buffer.data(), static_cast<std::size_t>(received));
        while (!data.empty()) {
            const std::size_t newline = data.find('\n');
            const std::string_view line = data.substr(0, newline);
            if (!line.empty()) {
                on_line(line);
            }
            if (newline == std::string_view::npos) {
                break;
            }
            data.remove_prefix(newline + 1);
        }
    }
    return datagrams;
}

}  // namespace telemetry
