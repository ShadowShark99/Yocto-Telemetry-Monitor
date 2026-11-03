// SPDX-License-Identifier: MIT
#include "sd_notify.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#include "unique_fd.hpp"

namespace telemetry::sd {

bool notify(std::string_view state) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (path == nullptr || (path[0] != '/' && path[0] != '@')) {
        return false;  // not under systemd (vsock addresses are not supported)
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::size_t length = std::strlen(path);
    if (length >= sizeof(address.sun_path)) {
        return false;
    }
    std::memcpy(address.sun_path, path, length);
    if (address.sun_path[0] == '@') {
        address.sun_path[0] = '\0';  // Linux abstract namespace socket
    }
    const auto address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + length);

    UniqueFd fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd) {
        return false;
    }
    ssize_t sent = 0;
    do {
        sent = ::sendto(fd.get(), state.data(), state.size(), MSG_NOSIGNAL,
                        reinterpret_cast<const sockaddr*>(&address), address_length);
    } while (sent < 0 && errno == EINTR);
    return sent == static_cast<ssize_t>(state.size());
}

std::optional<std::chrono::microseconds> watchdog_interval() {
    const char* usec = std::getenv("WATCHDOG_USEC");
    if (usec == nullptr || *usec == '\0') {
        return std::nullopt;
    }
    // WATCHDOG_PID, when present, says which process the watchdog is meant for.
    if (const char* pid = std::getenv("WATCHDOG_PID"); pid != nullptr && *pid != '\0') {
        if (std::strtoll(pid, nullptr, 10) != static_cast<long long>(::getpid())) {
            return std::nullopt;
        }
    }
    char* end = nullptr;
    const long long value = std::strtoll(usec, &end, 10);
    if (end == usec || *end != '\0' || value <= 0) {
        return std::nullopt;
    }
    return std::chrono::microseconds{value};
}

long long monotonic_usec() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000000LL + now.tv_nsec / 1000;
}

}  // namespace telemetry::sd
