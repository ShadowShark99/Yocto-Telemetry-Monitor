// SPDX-License-Identifier: MIT
//
// Reassembles newline-delimited packets from a byte stream (a serial port
// delivers bytes in arbitrary chunks, not whole lines).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace telemetry {

class LineBuffer {
public:
    explicit LineBuffer(std::size_t max_line_length = 512);

    /// Appends raw bytes and returns every line they complete.
    /// Lines end at '\n'; a trailing '\r' is dropped and empty lines are
    /// skipped. A line longer than max_line_length is discarded up to the
    /// next '\n' and counted in overflows(), so line noise or a stuck
    /// transmitter cannot grow memory without bound.
    std::vector<std::string> feed(std::string_view bytes);

    /// Drops any partial line, e.g. after the device was reopened.
    void clear() noexcept;

    std::size_t pending_bytes() const noexcept { return buffer_.size(); }
    std::uint64_t overflows() const noexcept { return overflows_; }

private:
    std::string buffer_;
    std::size_t max_line_length_;
    bool discarding_ = false;
    std::uint64_t overflows_ = 0;
};

}  // namespace telemetry
