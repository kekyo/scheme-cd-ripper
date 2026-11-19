// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include "terminal_input.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <deque>
#include <iostream>
#include <optional>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

struct TerminalInput {
    bool probed = false;
    bool supported = false;
    bool device_attributes_received = false;
    std::optional<bool> graphics_reply;
    std::string image_id;
    std::string escape;
    std::deque<char> pending;
    cc_t erase = 127;
    cc_t kill = 21;
};

static TerminalInput terminal_input;
static volatile sig_atomic_t probe_signal = 0;

static void defer_probe_signal(int signal) {
    probe_signal = signal;
}

// Delay termination/suspension only for the bounded query, so its terminal
// settings are restored before the original signal action takes effect. This
// also covers process-directed signals delivered to another library thread.
struct ProbeSignals {
    static constexpr std::array<int, 5> signals{SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};
    std::array<struct sigaction, signals.size()> previous{};
    std::array<bool, signals.size()> installed{};
    bool ready = true;

    ProbeSignals() {
        struct sigaction action{};
        action.sa_handler = defer_probe_signal;
        ::sigemptyset(&action.sa_mask);
        for (size_t index = 0; index < signals.size(); ++index) {
            if (::sigaction(signals[index], nullptr, &previous[index]) != 0) {
                ready = false;
                break;
            }
            if (previous[index].sa_handler == SIG_IGN) continue;
            if (::sigaction(signals[index], &action, nullptr) != 0) {
                ready = false;
                break;
            }
            installed[index] = true;
        }
    }

    ~ProbeSignals() {
        for (size_t index = 0; index < signals.size(); ++index) {
            if (installed[index]) ::sigaction(signals[index], &previous[index], nullptr);
        }
        const int signal = probe_signal;
        probe_signal = 0;
        if (signal != 0) ::raise(signal);
    }
};

// This descriptor is opened separately so O_NONBLOCK never changes stdin or
// stdout. TCSANOW preserves queued input, unlike TCSAFLUSH.
struct ProbeTerminal {
    int fd;
    termios original;

    ~ProbeTerminal() {
        while (::tcsetattr(fd, TCSANOW, &original) < 0 && errno == EINTR) {}
        ::close(fd);
    }
};

static void retain_escape(TerminalInput& input) {
    input.pending.insert(input.pending.end(), input.escape.begin(), input.escape.end());
    input.escape.clear();
}

// Parse replies incrementally because a PTY or SSH connection can split them
// at any byte. Other input, including ordinary keyboard escape sequences, is
// retained for the next prompt. Keeping this parser after a timeout also lets
// the prompt discard late replies instead of treating them as a selection.
static void receive_terminal_byte(TerminalInput& input, char byte) {
    if (input.escape.empty() && byte != '\x1b') {
        input.pending.push_back(byte);
        return;
    }
    input.escape += byte;
    const auto size = input.escape.size();
    if (size < 2) return;
    if (size > 4096) {
        retain_escape(input);
        return;
    }
    if (input.escape[1] == '_') {
        if (size == 2) return;
        if (input.escape[2] != 'G') {
            retain_escape(input);
            return;
        }
        if (size >= 5 && input.escape.compare(size - 2, 2, "\x1b\\") == 0) {
            const auto separator = input.escape.find(';', 3);
            if (separator != std::string::npos) {
                const auto fields = "," + input.escape.substr(3, separator - 3) + ",";
                if (!input.device_attributes_received &&
                    fields.find(",i=" + input.image_id + ",") != std::string::npos) {
                    input.graphics_reply =
                        input.escape.substr(separator + 1, size - separator - 3) == "OK";
                }
            }
            input.escape.clear();
        } else if (byte == '\n') {
            retain_escape(input);
        }
        return;
    }
    if (input.escape[1] == '[') {
        if (size == 2) return;
        if (input.escape[2] == '?') {
            if (size == 3) return;
            if (byte == 'c') {
                input.device_attributes_received = true;
                input.escape.clear();
                return;
            }
            if ((byte >= '0' && byte <= '9') || byte == ';') return;
        }
    }
    retain_escape(input);
}

// Silent or disconnected terminals must not hold up the CLI indefinitely.
// Share one monotonic deadline across writes, reads, and interrupted waits so
// partial replies cannot restart the timeout. select() blocks until readiness.
static bool wait_for_terminal(
    int fd, bool writable, std::chrono::steady_clock::time_point deadline) {
    while (probe_signal == 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(remaining / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(remaining % 1000000);
        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(fd, &descriptors);
        const int result = ::select(fd + 1,
            writable ? nullptr : &descriptors, writable ? &descriptors : nullptr,
            nullptr, &timeout);
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && probe_signal == 0;
    }
    return false;
}

static void query_kitty_graphics(int fd, TerminalInput& input) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    // a=q neither displays nor stores the image. DA1 is an ordered barrier:
    // its reply without a preceding graphics reply means no Kitty support.
    // https://sw.kovidgoyal.net/kitty/graphics-protocol/#querying-support-and-available-transmission-mediums
    const auto query = "\x1b_Gi=" + input.image_id +
        ",s=1,v=1,a=q,t=d,f=32;AAAAAA==\x1b\\\x1b[c";
    size_t written = 0;
    while (written < query.size() && wait_for_terminal(fd, true, deadline)) {
        const auto count = ::write(fd, query.data() + written, query.size() - written);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) break;
        written += static_cast<size_t>(count);
    }
    while (written == query.size() && !input.device_attributes_received &&
        wait_for_terminal(fd, false, deadline)) {
        char bytes[1024];
        const auto count = ::read(fd, bytes, sizeof(bytes));
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) break;
        for (ssize_t index = 0; index < count; ++index) {
            receive_terminal_byte(input, bytes[index]);
        }
    }
    input.supported = input.graphics_reply.value_or(false);
}

bool cdrip::terminal::probe_kitty_graphics() {
    auto& input = terminal_input;
    if (input.probed) return input.supported;
    input.probed = true;

    struct stat in_stat{}, out_stat{};
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO) ||
        ::fstat(STDIN_FILENO, &in_stat) != 0 || ::fstat(STDOUT_FILENO, &out_stat) != 0 ||
        in_stat.st_rdev != out_stat.st_rdev) return false;
    char path[1024];
    if (::ttyname_r(STDOUT_FILENO, path, sizeof(path)) != 0) return false;
    const int fd = ::open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    if (fd >= FD_SETSIZE) {
        ::close(fd);
        return false;
    }
    termios original{};
    if (::tcgetattr(fd, &original) != 0) {
        ::close(fd);
        return false;
    }
    const ProbeSignals signals;
    const ProbeTerminal terminal{fd, original};
    if (!signals.ready) return false;
    auto query_mode = original;
    query_mode.c_lflag &= ~(ICANON | ECHO | ECHONL);
    query_mode.c_cc[VMIN] = 1;
    query_mode.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &query_mode) != 0) return false;
    input.erase = original.c_cc[VERASE];
    input.kill = original.c_cc[VKILL];
    input.image_id = std::to_string(static_cast<unsigned>(::getpid()));
    std::cout.flush();
    query_kitty_graphics(fd, input);
    return input.supported;
}

bool cdrip::terminal::read_console_line(std::string& line) {
    auto& input = terminal_input;
    if (input.image_id.empty()) return static_cast<bool>(std::getline(std::cin, line));
    line.clear();
    while (true) {
        if (input.pending.empty()) {
            char byte;
            if (std::cin.get(byte)) {
                receive_terminal_byte(input, byte);
            } else {
                retain_escape(input);
                if (input.pending.empty()) return !line.empty();
            }
        }
        if (input.pending.empty()) continue;
        const auto byte = input.pending.front();
        input.pending.pop_front();
        if (byte == '\n') return true;
        if (static_cast<unsigned char>(byte) == input.erase) {
            if (!line.empty()) line.pop_back();
        } else if (static_cast<unsigned char>(byte) == input.kill) {
            line.clear();
        } else {
            line += byte;
        }
    }
}

bool cdrip::terminal::has_buffered_console_input() {
    return std::find(terminal_input.pending.begin(), terminal_input.pending.end(), '\n') !=
        terminal_input.pending.end();
}
