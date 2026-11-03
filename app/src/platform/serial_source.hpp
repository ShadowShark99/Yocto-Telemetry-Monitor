// SPDX-License-Identifier: MIT
//
// Reads packet lines from a serial device (or anything that can be opened
// and read, such as a pty). The device may disappear (USB adapter unplugged);
// the daemon then closes it and retries periodically.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "telemetry/line_buffer.hpp"
#include "unique_fd.hpp"

namespace telemetry {

class SerialSource {
public:
    using LineHandler = std::function<void(std::string_view line)>;

    SerialSource(std::string device, std::uint32_t baud);

    bool open(std::string& error);
    void close() noexcept;
    bool is_open() const noexcept { return static_cast<bool>(fd_); }
    int fd() const noexcept { return fd_.get(); }

    const std::string& device() const noexcept { return device_; }
    std::uint32_t baud() const noexcept { return baud_; }

    /// Reads all available bytes and passes complete lines on. Returns false
    /// if the device hung up or failed; the caller should close() it.
    bool read_lines(const LineHandler& on_line, std::string& error);

    std::uint64_t overflows() const noexcept { return buffer_.overflows(); }

private:
    std::string device_;
    std::uint32_t baud_;
    UniqueFd fd_;
    LineBuffer buffer_;
};

}  // namespace telemetry
