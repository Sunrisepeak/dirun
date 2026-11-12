// The console driver's semantics, one case per behaviour the frozen Python
// suite pinned (tools/reference/tests/test_console.py). The pty itself is
// exercised for real where the system has one; elsewhere those cases skip.
#if !defined(_WIN32)
#include <cerrno>
#include <unistd.h>
#endif
import std;
import dirun.console;
import dirun.testing;

namespace c = dirun::console;

namespace {

// Replays a scripted byte stream; the machine cannot tell it from a pty.
class FakeChannel final : public c::Channel {
public:
    explicit FakeChannel(std::vector<std::string> chunks) : chunks_(std::move(chunks)) {}

    std::string read_until(const std::string& pattern, double) override {
        const std::regex rx{pattern};
        while (true) {
            std::smatch match;
            if (std::regex_search(buffer_, match, rx)) {
                auto consumed = buffer_.substr(0, match.position() + match.length());
                buffer_.erase(0, match.position() + match.length());
                return consumed;
            }
            if (at_ == chunks_.size())
                throw c::ConsoleError{"pattern '" + pattern + "' not seen before timeout"};
            buffer_ += chunks_[at_++];
        }
    }

    void send(const std::string& line, bool = true) override { sent_.push_back(line); }

    const std::vector<std::string>& sent() const { return sent_; }

private:
    std::vector<std::string> chunks_;
    std::size_t at_ = 0;
    std::string buffer_;
    std::vector<std::string> sent_;
};

std::string slurp(const std::string& path) {
    std::ifstream file{path};
    return std::string{std::istreambuf_iterator<char>{file}, {}};
}

}  // namespace

int main() {
    using namespace dirun::testing;

    "the default login script logs a login-style image in"_test = [] {
        FakeChannel channel{{"Welcome\r\n", "box login: ", "Password: ", "# "}};
        c::ExpectMachine{c::default_login_script()}.run(channel);
        expect(channel.sent().size() == 2);
        expect(channel.sent()[0] == "root" && channel.sent()[1] == "root");
    };

    "a custom script matches and sends in order"_test = [] {
        const c::ExpectMachine machine{std::vector<c::ExpectStep>{{"user:", "alice", 30}, {">", "run", 30}}};
        FakeChannel channel{{"user: ", "> "}};
        machine.run(channel);
        expect(channel.sent().size() == 2);
        expect(channel.sent()[0] == "alice" && channel.sent()[1] == "run");
    };

    "a pattern that never appears is an error"_test = [] {
        const c::ExpectMachine machine{std::vector<c::ExpectStep>{{"never", "x", 0.5}}};
        FakeChannel channel{{"garbage"}};
        bool threw = false;
        try { machine.run(channel); } catch (const c::ConsoleError&) { threw = true; }
        expect(threw);
    };

    "a JSON script loads with its send and timeout"_test = [] {
        const auto dir = std::filesystem::temp_directory_path() / "dirun-console-test";
        std::filesystem::create_directories(dir);
        const auto path = (dir / "script.json").string();
        std::ofstream{path} << R"([{"expect": "a", "send": "b", "timeout": 3}])";
        std::string error;
        const auto steps = c::load_script(path, error);
        expect(steps.has_value() && !error.empty() == false);
        expect(steps->size() == 1);
        expect((*steps)[0].expect == "a" && (*steps)[0].send == "b");
        expect((*steps)[0].timeout_seconds == 3.0);
        std::filesystem::remove_all(dir);
    };

    "a JSON script that is not the right shape is refused"_test = [] {
        const auto dir = std::filesystem::temp_directory_path() / "dirun-console-test";
        std::filesystem::create_directories(dir);
        const auto path = (dir / "bad.json").string();
        std::ofstream{path} << R"({"expect": "a"})";
        std::string error;
        expect(!c::load_script(path, error).has_value());
        expect(!error.empty());
        std::filesystem::remove_all(dir);
    };

    "no script path is the default login script"_test = [] {
        std::string error;
        const auto steps = c::load_script(std::nullopt, error);
        expect(steps.has_value());
        expect(steps->size() == 3);
        expect((*steps)[0].expect == "login:");
    };

    "the markers carry the output and the rc, prelude first"_test = [] {
        FakeChannel channel{{"stty -echo\r\n",                       // the prelude's echo
                             "echo @@BEGIN@@\r\n",                   // the marker's echo
                             "line1\r\nline2\r\n",                   // the program's stdout
                             "@@STATUS=3@@\r\n# "}};
        const auto marked = c::run_marker_command(channel, "@@BEGIN@@-command", 5.0);
        expect(marked.rc == 3);
        expect(marked.output == "line1\nline2");
        expect(channel.sent().size() == 2);  // the prelude, then the wrapped line
        expect(channel.sent()[0] == "stty -echo");
        expect(channel.sent()[1] == "@@BEGIN@@-command");
    };

    "a missing BEGIN marker is an error"_test = [] {
        FakeChannel channel{{"no markers here"}};
        bool threw = false;
        try { (void)c::run_marker_command(channel, "cmd", 0.0); } catch (const c::ConsoleError&) { threw = true; }
        expect(threw);
    };

    "a missing STATUS marker is an error"_test = [] {
        FakeChannel channel{{"@@BEGIN@@\r\noutput but no status\r\n"}};
        bool threw = false;
        try { (void)c::run_marker_command(channel, "cmd", 0.0); } catch (const c::ConsoleError&) { threw = true; }
        expect(threw);
    };

#if !defined(_WIN32)  // the pty driver: a posix face
    "a stream that ends without the pattern is an error"_test = [] {
        c::PtyDriver driver{{"/usr/bin/true"}};
        bool threw = false;
        try { (void)driver.read_until("anything", 2.0); } catch (const c::ConsoleError&) { threw = true; }
        expect(threw);
    };

    "a line written comes back over the pty"_test = [] {
        c::PtyDriver driver{std::vector<std::string>{"/bin/cat"}};
        driver.send("ping", false);
        const auto got = driver.read_until("ping", 5.0);
        expect(got.find("ping") != std::string::npos);
    };

    "a long send is throttled between chunks"_test = [] {
        c::PtyDriver driver{std::vector<std::string>{"/bin/cat"}};
        int pauses = 0;
        const auto real = c::PtyDriver::throttle_sleep;
        c::PtyDriver::throttle_sleep = [&](double) { ++pauses; };
        driver.send(std::string(200, 'x'));  // four chunks: three pauses between them
        c::PtyDriver::throttle_sleep = real;
        expect(pauses == 3);
    };

    "a closed master fd reads as a closed console (EBADF)"_test = [] {
        c::PtyDriver driver{std::vector<std::string>{"/bin/sleep", "30"}};
        ::close(driver.master_fd());  // the fd goes bad while the child still runs
        bool threw = false;
        std::string what;
        try { (void)driver.read_until("anything", 2.0); }
        catch (const c::ConsoleError& error) { threw = true; what = error.what(); }
        expect(threw);
        expect(what.find("console closed: Bad file descriptor") != std::string::npos) << what;
    };

    "a dead far end reads as a closed console (EIO)"_test = [] {
        c::PtyDriver driver{std::vector<std::string>{"/bin/cat"}};
        c::set_read_error_probe([] { return EIO; });
        driver.send("x", false);  // make the master readable first
        bool threw = false;
        std::string what;
        try { (void)driver.read_until("anything", 2.0); }
        catch (const c::ConsoleError& error) { threw = true; what = error.what(); }
        c::set_read_error_probe(nullptr);
        expect(threw);
        expect(what.find("console closed: Input/output error") != std::string::npos) << what;
    };

    "a program that cannot start says so, not a closed console"_test = [] {
        bool threw = false;
        std::string what;
        try { c::PtyDriver driver{std::vector<std::string>{"/nonexistent/dirun-probe-binary"}}; }
        catch (const c::spawn_error& error) { threw = true; what = error.what(); }
        expect(threw);
        expect(what.find("cannot start") != std::string::npos) << what;
    };

    "closing ends the child's whole group and is repeatable"_test = [] {
        c::PtyDriver driver{std::vector<std::string>{"/bin/sleep", "30"}};
        expect(driver.child_alive());
        driver.close();
        driver.close();  // idempotent
        expect(!driver.child_alive());
    };
#else
    [] { skip("the pty console driver is not built for this system yet"); }();
#endif

    return report();
}
