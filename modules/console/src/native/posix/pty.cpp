// The pty on a POSIX system: openpty, a child in its own session, a poll
// loop, a process-group end. Kept thin on purpose --- everything generic lives
// in the module above it.
#include "../../pty_face.hpp"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <poll.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>

#include <csignal>
#include <cstring>

#include <chrono>
#include <thread>

namespace dirun::console::native {

namespace {

std::string errno_text(int code) { return std::string(std::strerror(code)); }

channel_error closed_error(int code) {
    return channel_error("console closed: " + errno_text(code) + " (errno " + std::to_string(code) + ")");
}

// A test seam: when set, the next read fails with this errno (the injected EIO
// form of a dead channel; the EBADF form is real --- close the master fd).
int (*g_read_error_probe)() = nullptr;

}  // namespace

struct pty_state {
    int master = -1;
    pid_t child = -1;
    bool closed = false;
};

pty_state* pty_open(const std::vector<std::string>& argv) {
    int pair[2];
    if (::openpty(&pair[0], &pair[1], nullptr, nullptr, nullptr) != 0)
        throw channel_error("cannot allocate pty: " + errno_text(errno));
    const int master = pair[0];
    const int slave = pair[1];

    // The exec report: a pipe whose far end closes when exec succeeds, and
    // carries the errno when it does not --- so a program that cannot start
    // says so (the runner's 126), instead of dying as a silent console end.
    int report[2];
    if (::pipe(report) != 0) {
        const int code = errno;
        ::close(master);
        ::close(slave);
        throw channel_error("cannot allocate the exec report pipe: " + errno_text(code));
    }
    ::fcntl(report[1], F_SETFD, FD_CLOEXEC);

    std::vector<char*> words;
    words.reserve(argv.size() + 1);
    for (const auto& arg : argv) words.push_back(const_cast<char*>(arg.c_str()));
    words.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int code = errno;
        ::close(master);
        ::close(slave);
        ::close(report[0]);
        ::close(report[1]);
        throw spawn_error("fork failed: " + errno_text(code));
    }
    if (pid == 0) {
        // Own session, the slave as the controlling terminal, stdio on it.
        ::setsid();
        if (::ioctl(slave, TIOCSCTTY, 0) != 0
            || ::dup2(slave, 0) < 0 || ::dup2(slave, 1) < 0 || ::dup2(slave, 2) < 0) {
            const int code = errno;
            (void)!::write(report[1], &code, sizeof code);
            _exit(127);
        }
        if (slave > 2) ::close(slave);
        ::execvp(words[0], words.data());
        const int code = errno;
        (void)!::write(report[1], &code, sizeof code);
        _exit(127);  // exec failed: 127, the shell's "not found"
    }
    // Each fd closed exactly once: a double close raises EBADF here and masks
    // the real outcome.
    ::close(slave);
    ::close(report[1]);
    int exec_errno = 0;
    while (::read(report[0], &exec_errno, sizeof exec_errno) < 0 && errno == EINTR) {}
    ::close(report[0]);
    if (exec_errno != 0) {
        ::waitpid(pid, nullptr, 0);
        ::close(master);
        throw spawn_error("cannot start '" + argv.front() + "': " + errno_text(exec_errno));
    }

    auto* state = new pty_state;
    state->master = master;
    state->child = pid;
    return state;
}
read_once pty_read_once(pty_state& state, double timeout_seconds, std::string& into) {
    if (state.closed || state.master < 0)
        throw closed_error(EBADF);
    if (::waitpid(state.child, nullptr, WNOHANG) != 0)
        return read_once::ended;  // the child is gone

    if (g_read_error_probe) {
        const int code = g_read_error_probe();
        if (code != 0) throw closed_error(code);
    }

    struct pollfd waiter;
    waiter.fd = state.master;
    waiter.events = POLLIN;
    waiter.revents = 0;
    const int ready = ::poll(&waiter, 1, static_cast<int>(timeout_seconds * 1000.0));
    if (ready < 0)
        throw closed_error(errno);
    if (ready == 0)
        return read_once::waiting;

    char chunk[4096];
    const ssize_t got = ::read(state.master, chunk, sizeof chunk);
    if (got < 0)
        throw closed_error(errno);  // EIO on Linux once the far end died
    if (got == 0)
        return read_once::ended;
    into.append(chunk, static_cast<std::size_t>(got));
    return read_once::bytes;
}

void pty_write(pty_state& state, const std::string& bytes) {
    if (state.closed || state.master < 0)
        throw closed_error(EBADF);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(state.master, bytes.data() + written, bytes.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw closed_error(errno);
        }
        written += static_cast<std::size_t>(n);
    }
}

int pty_master_fd(const pty_state& state) { return state.master; }

bool pty_child_alive(const pty_state& state) {
    return ::waitpid(state.child, nullptr, WNOHANG) == 0;
}

void pty_close(pty_state*& handle) {
    if (handle == nullptr) return;
    pty_state& state = *handle;
    if (!state.closed) {
        state.closed = true;
        if (state.child > 0) {
            ::kill(-state.child, SIGTERM);  // the whole process group
            for (int waited = 0; waited < 50 && ::waitpid(state.child, nullptr, WNOHANG) == 0; ++waited)
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            if (::waitpid(state.child, nullptr, WNOHANG) == 0) {
                ::kill(-state.child, SIGKILL);
                ::waitpid(state.child, nullptr, 0);
            }
        }
        if (state.master >= 0) ::close(state.master);
        state.master = -1;
    }
    delete handle;
    handle = nullptr;
}

void set_read_error_probe(int (*probe)()) { g_read_error_probe = probe; }

}  // namespace dirun::console::native
