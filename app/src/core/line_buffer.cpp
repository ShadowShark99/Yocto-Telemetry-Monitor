// SPDX-License-Identifier: MIT
#include "telemetry/line_buffer.hpp"

#include <utility>

namespace telemetry {

LineBuffer::LineBuffer(std::size_t max_line_length) : max_line_length_(max_line_length) {
    buffer_.reserve(max_line_length_);
}

std::vector<std::string> LineBuffer::feed(std::string_view bytes) {
    std::vector<std::string> lines;
    for (const char c : bytes) {
        if (c == '\n') {
            if (discarding_) {
                discarding_ = false;
            } else {
                if (!buffer_.empty() && buffer_.back() == '\r') {
                    buffer_.pop_back();
                }
                if (!buffer_.empty()) {
                    lines.push_back(std::move(buffer_));
                }
            }
            buffer_.clear();
            continue;
        }
        if (discarding_) {
            continue;
        }
        if (buffer_.size() >= max_line_length_) {
            discarding_ = true;
            ++overflows_;
            buffer_.clear();
            continue;
        }
        buffer_.push_back(c);
    }
    return lines;
}

void LineBuffer::clear() noexcept {
    buffer_.clear();
    discarding_ = false;
}

}  // namespace telemetry
