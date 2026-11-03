// SPDX-License-Identifier: MIT
#include "serial_port.hpp"

#include <termios.h>

#include <cerrno>
#include <cstring>

namespace telemetry {
namespace {

bool baud_to_speed(std::uint32_t baud, speed_t& speed) noexcept {
    switch (baud) {
        case 9600: speed = B9600; return true;
        case 19200: speed = B19200; return true;
        case 38400: speed = B38400; return true;
        case 57600: speed = B57600; return true;
        case 115200: speed = B115200; return true;
        case 230400: speed = B230400; return true;
        case 460800: speed = B460800; return true;
        case 921600: speed = B921600; return true;
        default: return false;
    }
}

}  // namespace

bool configure_serial(int fd, std::uint32_t baud, std::string& error) {
    speed_t speed = B0;
    if (!baud_to_speed(baud, speed)) {
        error = "unsupported baud rate " + std::to_string(baud);
        return false;
    }

    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        error = std::string("tcgetattr: ") + std::strerror(errno);
        return false;
    }

    cfmakeraw(&tio);                // no echo, no line editing, 8 data bits, no parity
    tio.c_cflag |= CLOCAL | CREAD;  // ignore modem lines, enable the receiver
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | CRTSCTS);  // 1 stop bit, no hardware flow control
    // VMIN = 1 with O_NONBLOCK: read() returns -1/EAGAIN when no data is
    // waiting, so a return of 0 unambiguously means hang-up. (With VMIN = 0
    // a raw tty returns 0 for "nothing yet", which looks exactly like EOF.)
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0) {
        error = std::string("cfsetspeed: ") + std::strerror(errno);
        return false;
    }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        error = std::string("tcsetattr: ") + std::strerror(errno);
        return false;
    }
    tcflush(fd, TCIFLUSH);  // drop whatever accumulated before we were listening
    return true;
}

}  // namespace telemetry
