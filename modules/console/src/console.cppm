// Console driving: expect/send primitives over a byte channel (a pty or a test
// double). The default login script is one policy --- a login-style interactive
// image --- and any system replaces it whole through the runner's --expect; the
// driver itself knows no system.
//
// A signal that reaches the driver between reads surfaces as ConsoleInterrupted
// so the caller's cleanup runs and the exit code keeps signal semantics.
module;
#include "pty_face.hpp"
export module dirun.console;

import std;
import nlohmann.json;
import dirun.protocol;

export namespace dirun::console {

using dirun::console::native::channel_error;
using dirun::console::native::spawn_error;
using dirun::console::native::set_read_error_probe;  // the test seam

// The channel the expect machine drives. read_until(pattern, timeout) returns
// every byte consumed up to and including the first match; send(line) writes a
// line, throttled unless told otherwise.
class Channel {
public:
    virtual ~Channel() = default;
    virtual std::string read_until(const std::string& pattern, double timeout_seconds) = 0;
    virtual void send(const std::string& line, bool throttle = true) = 0;
};

class ConsoleError : public std::exception {
public:
    explicit ConsoleError(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

// A signal arrived while the console was being driven.
class ConsoleInterrupted : public std::exception {
public:
    explicit ConsoleInterrupted(int signum) : signum_(signum) {}
    int signum() const noexcept { return signum_; }
    const char* what() const noexcept override { return "console interrupted by a signal"; }

private:
    int signum_;
};

// One step of a console script: match this, then (unless empty) send that.
struct ExpectStep {
    std::string expect;             // a regex over the console stream
    std::string send;               // empty for "match only"
    double timeout_seconds = 60.0;
};

// The default bootstrap script: a login-style image (replaceable per system).
// This is the one place a login flow is named.
inline std::vector<ExpectStep> default_login_script() {
    return {
        {"login:", "root", 60.0},
        {"Password:", "root", 60.0},
        {"[#$] $", "", 60.0},
    };
}

// The expect script from a JSON array of {"expect": ..., "send": ...,
// "timeout": ...}; std::nullopt path is the default login script.
inline std::optional<std::vector<ExpectStep>> load_script(const std::optional<std::string>& path,
                                                          std::string& error) {
    if (!path) return default_login_script();
    std::ifstream file{*path};
    if (!file) {
        error = "cannot read the expect script: " + *path;
        return std::nullopt;
    }
    const nlohmann::json document = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_array()) {
        error = "the expect script must be a JSON array of {expect, send, timeout} objects";
        return std::nullopt;
    }
    std::vector<ExpectStep> steps;
    for (const auto& entry : document) {
        if (!entry.is_object() || !entry.contains("expect") || !entry["expect"].is_string()) {
            error = "the expect script must be a JSON array of {expect, send, timeout} objects";
            return std::nullopt;
        }
        ExpectStep step;
        step.expect = entry["expect"].get<std::string>();
        if (entry.contains("send") && entry["send"].is_string())
            step.send = entry["send"].get<std::string>();
        if (entry.contains("timeout") && entry["timeout"].is_number())
            step.timeout_seconds = entry["timeout"].get<double>();
        steps.push_back(std::move(step));
    }
    return steps;
}

class ExpectMachine {
public:
    explicit ExpectMachine(std::vector<ExpectStep> steps) : steps_(std::move(steps)) {}

    const std::vector<ExpectStep>& steps() const { return steps_; }

    void run(Channel& channel) const {
        for (const auto& step : steps_) {
            channel.read_until(step.expect, step.timeout_seconds);
            if (!step.send.empty()) channel.send(step.send);
        }
    }

private:
    std::vector<ExpectStep> steps_;
};

// What a marker-wrapped command produced: the program's stdout and its rc.
struct marker_result {
    std::string output;
    int rc = -1;
};

// Sends the wrapped command and gathers what the markers delimit. The
// `stty -echo` prelude goes out first, as its own line, so the tty stops
// echoing before the marker line arrives (otherwise the command echo lands
// between BEGIN and the program's output).
marker_result run_marker_command(Channel& channel, const std::string& command_line, double timeout_seconds);

// The pty driver: a child whose stdio is a serial console, killed as a group.
class PtyDriver final : public Channel {
public:
    static constexpr double send_delay_seconds = 0.02;  // the throttle, between chunks
    static constexpr std::size_t send_chunk = 64;

    // The pause between chunks; a test may replace it to count instead of wait.
    static inline std::function<void(double)> throttle_sleep = [](double seconds) {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    };

    explicit PtyDriver(const std::vector<std::string>& argv)
        : state_(native::pty_open(argv)) {}

    ~PtyDriver() override { close(); }
    PtyDriver(const PtyDriver&) = delete;
    PtyDriver& operator=(const PtyDriver&) = delete;

    // When set (the runner's signal guard sets it), a non-zero signum aborts
    // the read as ConsoleInterrupted instead of waiting the timeout out.
    static inline std::function<int()> interrupt_probe;

    std::string read_until(const std::string& pattern, double timeout_seconds) override {
        if (state_ == nullptr)
            throw ConsoleError("console closed: the driver was closed");
        const std::regex rx{pattern, std::regex_constants::multiline};
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::duration<double>(std::max(0.0, timeout_seconds));
        std::smatch match;
        while (!std::regex_search(buffer_, match, rx)) {
            if (interrupt_probe) {
                const int signum = interrupt_probe();
                if (signum != 0) throw ConsoleInterrupted{signum};
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (std::regex_search(buffer_, match, rx)) break;  // one final chance
                throw ConsoleError("pattern '" + pattern + "' not seen before timeout");
            }
            std::string chunk;
            try {
                const auto outcome = native::pty_read_once(*state_, 0.05, chunk);
                if (outcome == native::read_once::bytes) {
                    buffer_ += chunk;
                    continue;
                }
                if (outcome == native::read_once::ended) {
                    // The child is gone; what is buffered is all there is.
                    if (std::regex_search(buffer_, match, rx)) break;
                    throw ConsoleError("pattern '" + pattern
                                       + "' not seen (the console ended first)");
                }
            } catch (const native::channel_error& error) {
                throw ConsoleError(error.what());
            }
        }
        std::string consumed = buffer_.substr(0, match.position() + match.length());
        buffer_.erase(0, match.position() + match.length());
        return consumed;
    }

    void send(const std::string& line, bool throttle = true) override {
        if (state_ == nullptr)
            throw ConsoleError("console closed: the driver was closed");
        std::string bytes = line;
        bytes += '\n';
        try {
            for (std::size_t at = 0; at < bytes.size(); at += send_chunk) {
                const auto upto = std::min(send_chunk, bytes.size() - at);
                native::pty_write(*state_, bytes.substr(at, upto));
                if (throttle && at + send_chunk < bytes.size())
                    throttle_sleep(send_delay_seconds);
            }
        } catch (const native::channel_error& error) {
            throw ConsoleError(error.what());
        }
    }

    int master_fd() const { return state_ ? native::pty_master_fd(*state_) : -1; }
    bool child_alive() const { return state_ && native::pty_child_alive(*state_); }

    // Ends the child's whole process group and closes the master; idempotent.
    void close() {
        native::pty_close(state_);  // closes the group, frees, nulls the handle
    }


private:
    native::pty_state* state_ = nullptr;
    std::string buffer_;
};

}  // namespace dirun::console

// --- implementation ---------------------------------------------------------
namespace dirun::console {

marker_result run_marker_command(Channel& channel, const std::string& command_line,
                                 double timeout_seconds) {
    channel.send("stty -echo");
    PtyDriver::throttle_sleep(0.2);  // let the prelude land before the markers
    channel.send(command_line);

    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::duration<double>(std::max(0.0, timeout_seconds));
    channel.read_until(std::string{protocol::begin_marker},
                       std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count());
    const std::string consumed = channel.read_until(
        R"(@@STATUS=(\d+)@@)",
        std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count());

    const protocol::split_result split = protocol::split_output(consumed);
    if (split.rc < 0) throw ConsoleError("status marker missing");
    return {split.output, split.rc};
}

}  // namespace dirun::console
