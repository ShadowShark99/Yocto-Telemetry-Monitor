// SPDX-License-Identifier: MIT
//
// termios helpers shared by the daemon (reader) and simulator (writer).
#pragma once

#include <cstdint>
#include <string>

namespace telemetry {

/// Puts a tty into raw 8N1 mode at `baud` with no flow control
/// (VMIN = 1, VTIME = 0; open the device O_NONBLOCK). On failure, fills `error`.
bool configure_serial(int fd, std::uint32_t baud, std::string& error);

}  // namespace telemetry
