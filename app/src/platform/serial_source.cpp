// SPDX-License-Identifier: MIT
#include "serial_source.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#include "serial_port.hpp"

namespace telemetry {

SerialSource::SerialSource(std::string device, std::uint32_t baud) : device_(std::move(device)), baud_(baud) {}

bool SerialSource::open(std::string& error) {
    // O_NOCTTY: never let the device become our controlling terminal.
    UniqueFd fd(::open(device_.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
        error = device_ + ": " + std::strerror(errno);
        return false;
    }
    if (::isatty(fd.get())) {
        std::string why;
        if (!configure_serial(fd.get(), baud_, why)) {
            error = device_ + ": " + why;
            return false;
        }
    }
    fd_ = std::move(fd);
    buffer_.clear();
    return true;
}

void SerialSource::close() noexcept {
    fd_.reset();
    buffer_.clear();
}

bool SerialSource::read_lines(const LineHandler& on_line, std::string& error) {
    std::array<char, 512> chunk{};
    while (true) {
        const ssize_t n = ::read(fd_.get(), chunk.data(), chunk.size());
        if (n > 0) {
            for (const auto& line : buffer_.feed(std::string_view(chunk.data(), static_cast<std::size_t>(n)))) {
                on_line(line);
            }
            continue;
        }
        if (n == 0) {
            error = device_ + ": hung up";
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        error = device_ + ": " + std::strerror(errno);  // EIO when a USB adapter is pulled
        return false;
    }
}

}  // namespace telemetry
