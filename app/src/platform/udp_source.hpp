// SPDX-License-Identifier: MIT
//
// Non-blocking IPv4 UDP listener. Each datagram carries one or more packet
// lines separated by '\n'.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "unique_fd.hpp"

namespace telemetry {

class UdpSource {
public:
    using LineHandler = std::function<void(std::string_view line)>;

    static std::optional<UdpSource> open(const std::string& bind_address, std::uint16_t port, std::string& error);

    int fd() const noexcept { return fd_.get(); }
    const std::string& bind_address() const noexcept { return bind_address_; }
    std::uint16_t port() const noexcept { return port_; }

    /// Reads queued datagrams (at most a bounded batch per call, so a flood
    /// cannot starve the timer and signal handling) and passes each line on.
    /// Returns the number of datagrams read.
    std::size_t drain(const LineHandler& on_line);

    std::uint64_t oversized_datagrams() const noexcept { return oversized_; }

private:
    UdpSource(UniqueFd fd, std::string bind_address, std::uint16_t port)
        : fd_(std::move(fd)), bind_address_(std::move(bind_address)), port_(port) {}

    UniqueFd fd_;
    std::string bind_address_;
    std::uint16_t port_ = 0;
    std::uint64_t oversized_ = 0;
};

}  // namespace telemetry
