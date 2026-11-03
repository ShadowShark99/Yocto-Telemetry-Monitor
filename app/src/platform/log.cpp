// SPDX-License-Identifier: MIT
#include "log.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace telemetry::log {
namespace {

LogLevel g_level = LogLevel::Info;
bool g_journal = false;

// syslog(3) priorities, as understood by journald's stream protocol.
int priority(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return 7;
        case LogLevel::Info: return 6;
        case LogLevel::Warning: return 4;
        case LogLevel::Error: return 3;
    }
    return 6;
}

const char* label(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info ";
        case LogLevel::Warning: return "warn ";
        case LogLevel::Error: return "error";
    }
    return "info ";
}

// systemd sets JOURNAL_STREAM=<device>:<inode> for the stream it connected to
// stdout/stderr. Compare it against stderr so a redirected stderr (e.g. a
// shell running under a service) is not mistaken for the journal.
bool stderr_is_journal() {
    const char* stream = std::getenv("JOURNAL_STREAM");
    if (stream == nullptr || *stream == '\0') {
        return false;
    }
    unsigned long long device = 0;
    unsigned long long inode = 0;
    if (std::sscanf(stream, "%llu:%llu", &device, &inode) != 2) {
        return false;
    }
    struct stat st {};
    if (fstat(STDERR_FILENO, &st) != 0) {
        return false;
    }
    return static_cast<unsigned long long>(st.st_dev) == device &&
           static_cast<unsigned long long>(st.st_ino) == inode;
}

}  // namespace

void init() {
    g_journal = stderr_is_journal();
}

void set_level(LogLevel level) noexcept {
    g_level = level;
}

LogLevel level() noexcept {
    return g_level;
}

bool enabled(LogLevel level) noexcept {
    return static_cast<int>(level) >= static_cast<int>(g_level);
}

void write(LogLevel level, const char* fmt, ...) {
    if (!enabled(level)) {
        return;
    }

    std::array<char, 1024> line{};
    int used = 0;
    if (g_journal) {
        used = std::snprintf(line.data(), line.size(), "<%d>", priority(level));
    } else {
        timespec now{};
        clock_gettime(CLOCK_REALTIME, &now);
        tm local{};
        localtime_r(&now.tv_sec, &local);
        std::array<char, 32> stamp{};
        std::strftime(stamp.data(), stamp.size(), "%Y-%m-%d %H:%M:%S", &local);
        used = std::snprintf(line.data(), line.size(), "%s.%03ld [%s] ", stamp.data(), now.tv_nsec / 1000000L,
                             label(level));
    }
    if (used < 0) {
        return;
    }

    const auto room = line.size() - 1;  // keep one byte for '\n'
    std::size_t length = static_cast<std::size_t>(used) < room ? static_cast<std::size_t>(used) : room;
    va_list args;
    va_start(args, fmt);
    const int body = std::vsnprintf(line.data() + length, line.size() - length, fmt, args);
    va_end(args);
    if (body > 0) {
        length += static_cast<std::size_t>(body);
        if (length > room - 1) {
            length = room - 1;  // message was truncated
        }
    }
    line[length++] = '\n';

    // One write() per line keeps lines intact if several processes share stderr.
    const ssize_t ignored = ::write(STDERR_FILENO, line.data(), length);
    (void)ignored;
}

}  // namespace telemetry::log
