// The runner: QEMU's command line, the write-isolated overlay, the files disk, the guest and payload
// modes, and the exit codes (docs/contract.md):
//
//     0-255  the program's own status (guest), or the payload's (payload), passed through
//     124    the run's time limit expired first
//     125    the console ended, or a marker never came
//     126    a prerequisite is unusable (qemu, the image, the profile, a file to deliver)
//     130    interrupted (SIGINT); 143 terminated (SIGTERM); the emulator is ended either way
module;
#include <csignal>
export module dirun.runner;

import std;
import mcpplibs.os;
import mcpplibs.mtest.e2e;
import nlohmann.json;
import dirun.profile;
import dirun.protocol;

export namespace dirun::runner {

namespace os = mcpplibs::os;
namespace e2e = mcpplibs::mtest::e2e;

inline constexpr int exit_timeout = 124;
inline constexpr int exit_no_console = 125;
inline constexpr int exit_unusable = 126;

class error : public std::exception {
public:
    error(std::string cause, int code, std::string remedy = {})
        : text_(remedy.empty() ? cause : cause + ". To fix: " + remedy), code_(code) {}
    int code() const noexcept { return code_; }
    const char* what() const noexcept override { return text_.c_str(); }
private:
    std::string text_;
    int code_;
};

// How the system's tools run (qemu-img): one seam, so a test answers
// for them without any being present.
struct tool_result {
    int status = -1;
    std::string out, err;
};
class tools {
public:
    virtual ~tools() = default;
    virtual tool_result run(const std::vector<std::string>& argv) = 0;
    virtual bool has(const std::string& program) = 0;
};
class system_tools final : public tools {
public:
    tool_result run(const std::vector<std::string>& argv) override {
        os::process::spawn_options o;
        o.program = os::fs::from_utf8(argv.front());
        o.args.assign(argv.begin() + 1, argv.end());
        o.in = os::process::stdio::null;
        auto r = os::process::run(o);
        if (!r)
            return {127, "", r.error().message()};
        return {r->status.shell_code(), std::move(r->out), std::move(r->err)};
    }
    bool has(const std::string& program) override {
        return os::env::find_executable(program).has_value();
    }
};

struct file_delivery {
    std::string host, guest;
};

enum class accel { auto_, kvm, tcg };

struct options {
    profile::profile prof = profile::builtin();
    std::map<std::string, std::string> vars;  // --var NAME=VALUE, for {NAME} in the profile
    std::string image;                         // guest mode
    std::string payload;                       // payload mode
    std::vector<std::string> command;          // guest mode: the command's words
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<file_delivery> files;
    // The test protocol of the program that started this one (MTEST_PROTOCOL and MTEST_REPORT set):
    // the command speaks it in the guest, and its events come back to `report'.
    struct test_protocol {
        std::string version, report, mode, select;
    };
    std::optional<test_protocol> protocol;
    std::optional<std::string> arch, machine, cpu;
    std::optional<int> memory, smp;
    accel acceleration = accel::auto_;
    double timeout = 300;
    std::string trace;  // a file the console's transcript is written to, however the run ends
    // The emulator's command in place of `qemu-system-<arch>' found on PATH (--emulator: a runner
    // that installs its emulator names it). Test seams: the tools, a fixed work directory (removed
    // all the same).
    std::vector<std::string> emulator;
    tools* with_tools = nullptr;
    std::optional<std::filesystem::path> workdir;
};

struct result {
    int status = 0;
    std::string output;
    int signal = 0;  // the run was ended by this signal; status is 128 + it
};

result run(const options& o);

// The emulator's words for `o', its boot disk and file disk given (guest mode).
std::vector<std::string> qemu_words(const options& o, const std::string& arch);

}  // namespace dirun::runner

namespace dirun::runner {

namespace {

volatile std::sig_atomic_t g_signal = 0;
extern "C" void on_signal(int s) { g_signal = s; }

struct signals {
    using handler = void (*)(int);
    handler prev_int, prev_term;
    signals() {
        g_signal = 0;
        prev_int = std::signal(SIGINT, on_signal);
        prev_term = std::signal(SIGTERM, on_signal);
    }
    ~signals() {
        std::signal(SIGINT, prev_int);
        std::signal(SIGTERM, prev_term);
    }
};

struct interrupted {
    int signal;
};

class workdir {
public:
    explicit workdir(const std::optional<std::filesystem::path>& fixed) {
        static std::atomic<unsigned> serial{0};
        path_ = fixed ? *fixed
                      : std::filesystem::temp_directory_path() /
                            std::format("dirun-{}-{}", os::process::self() ? os::process::self()->id : 0,
                                        serial.fetch_add(1));
        std::filesystem::create_directories(path_);
    }
    ~workdir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

std::string str(const std::filesystem::path& p) { return os::fs::utf8(p); }

std::string expanded(const std::string& text, const std::map<std::string, std::string>& vars) {
    auto e = profile::expand(text, vars);
    if (!e)
        throw error("the profile: " + e.error(), exit_unusable);
    return *e;
}

std::string probe_format(const std::string& image, tools& t) {
    if (!t.has("qemu-img"))
        throw error("qemu-img is not on PATH", exit_unusable, "install qemu (qemu-img ships with it)");
    auto info = t.run({"qemu-img", "info", "--output=json", image});
    if (info.status != 0)
        throw error("qemu-img cannot read " + image + ": " + info.err, exit_unusable,
                    "check the image with `qemu-img info <image>`");
    // The image's own format is the top-level `format'; the layers below it (the `file' protocol
    // that newer qemu-img lists first among `children') have formats of their own.
    auto j = nlohmann::json::parse(info.out, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("format") || !j["format"].is_string())
        throw error("qemu-img names no format for " + image, exit_unusable,
                    "check the image with `qemu-img info <image>`");
    return j["format"].get<std::string>();
}

// The files gathered in a directory that the emulator presents as a read-only FAT disk (QEMU's
// `fat:' driver: no tool of the host's, the same on every host); mounting it is the profile's
// [files] mount. The disk has one partition.
std::string files_dir(const std::vector<file_delivery>& files, const std::filesystem::path& dir) {
    const auto root = dir / "files";
    std::filesystem::create_directories(root);
    for (const auto& f : files) {
        auto g = f.guest;
        while (!g.empty() && g.front() == '/')
            g.erase(g.begin());
        const auto to = root / os::fs::from_utf8(g);
        std::error_code ec;
        std::filesystem::create_directories(to.parent_path());
        std::filesystem::copy_file(os::fs::from_utf8(f.host), to, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            throw error("the file to deliver could not be copied: " + f.host + " (" + ec.message() + ")", exit_unusable);
    }
    return str(root);
}

std::vector<std::string> accel_words(accel a) {
    switch (a) {
    case accel::kvm: return {"-accel", "kvm"};
    case accel::tcg: return {"-accel", "tcg"};
    case accel::auto_: break;
    }
    // The host's own accelerator first, the emulator's translator when it is refused.
    const auto host = os::host();
    std::string native = host == os::platform::windows ? "whpx" : host == os::platform::macos ? "hvf" : "kvm";
    return {"-accel", native, "-accel", "tcg"};
}

struct clock {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double limit;
    double left() const {
        return limit - std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
};

// Waits for `pattern' (or, when given, `alternative') within `seconds' and the run's limit,
// noticing a signal between slices. Returns what was consumed and which matched (1 or 2).
std::pair<std::string, int> await(e2e::session& s, const std::string& pattern, const std::string& alternative,
                                  double seconds, const clock& c, const char* during) {
    const std::string either = alternative.empty() ? pattern : "(" + pattern + ")|(" + alternative + ")";
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::min(seconds, std::max(0.0, c.left())));
    for (;;) {
        if (g_signal)
            throw interrupted{int(g_signal)};
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) {
            if (c.left() <= 0)
                throw error(std::format("the time limit expired {}", during), exit_timeout,
                            "raise --timeout to cover the boot");
            throw error(std::format("the console never showed /{}/ {}", pattern, during), exit_no_console,
                        "check the profile's patterns against the console's output");
        }
        const auto slice = std::min<std::chrono::milliseconds>(
            std::chrono::milliseconds{250}, std::chrono::duration_cast<std::chrono::milliseconds>(end - now));
        if (auto got = s.expect(either, slice)) {
            int which = 1;
            if (!alternative.empty()) {
                std::smatch m;
                const std::regex rx(either, std::regex::ECMAScript);
                if (std::regex_search(*got, m, rx) && m.size() > 2 && m[2].matched && !m[1].matched)
                    which = 2;
            }
            return {*got, which};
        }
        if (s.ended())
            throw error(std::format("the console ended {}", during), exit_no_console,
                        "the emulator stopped; its last output:\n" + std::string(s.transcript().substr(
                                                                          s.transcript().size() > 800 ? s.transcript().size() - 800 : 0)));
    }
}

// Writes the session's transcript to the trace file when the run ends, however it ends.
struct tracer {
    const std::string& file;
    e2e::session& s;
    const std::vector<std::string>& argv;
    ~tracer() {
        if (file.empty())
            return;
        if (std::ofstream out(os::fs::from_utf8(file), std::ios::binary); out) {
            out << "$";
            for (const auto& w : argv)
                out << ' ' << w;
            out << '\n' << s.transcript();
        }
    }
};

void say(e2e::session& s, const std::string& text, bool line, int pace = 0) {
    if (pace <= 0) {
        if (line)
            s.send_line(text);
        else
            s.send(text);
        return;
    }
    for (char ch : text + (line ? "\n" : "")) {
        s.send(std::string(1, ch));
        std::this_thread::sleep_for(std::chrono::milliseconds{pace});
    }
}

// What the command is given beyond its words: a program of this machine delivered and named by its
// copy, and the test protocol's variables (docs/contract.md).
struct delivery {
    std::vector<std::string> command;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<file_delivery> files;
    std::optional<std::string> report;  // the host's MTEST_REPORT the guest's events are appended to
};

delivery deliver(const options& o) {
    const auto& p = o.prof;
    delivery d{o.command, o.env, o.files, std::nullopt};
    auto offered = [&](const std::string& host, const std::string& name) {
        if (p.files_at.empty())
            throw error("the profile " + p.origin + " states no [files] at: a file of this machine has no place "
                        "in the guest", exit_unusable);
        d.files.push_back({host, name});
        return p.files_at + "/" + name;
    };
    // A program offered from here is run from its copy; a name that is no file here is the guest's.
    if (std::error_code ec; !d.command.empty() &&
                            std::filesystem::is_regular_file(os::fs::from_utf8(d.command.front()), ec))
        d.command.front() = offered(d.command.front(), os::fs::utf8(os::fs::from_utf8(d.command.front()).filename()));
    if (o.protocol && p.events) {
        const auto& t = *o.protocol;
        d.report = t.report;
        d.env.emplace_back("MTEST_PROTOCOL", t.version);
        d.env.emplace_back("MTEST_REPORT", p.events->report);
        if (!t.mode.empty())
            d.env.emplace_back("MTEST_MODE", t.mode);
        if (!t.select.empty())
            d.env.emplace_back("MTEST_SELECT", offered(t.select, "mtest-select.txt"));
    }
    return d;
}

result run_guest(const options& o, tools& t, const clock& c) {
    const auto& p = o.prof;
    const auto image = o.image.empty() ? p.vm.image : o.image;
    if (image.empty())
        throw error("no image: name one with --image, or a profile whose [machine] states its image", exit_unusable);
    if (std::error_code ec; !std::filesystem::exists(os::fs::from_utf8(image), ec))
        throw error("the image does not exist: " + image, exit_unusable, "pass an existing disk image to --image");
    if (p.cmd.line.empty())
        throw error("the profile " + p.origin + " has no [command]: it cannot run a command", exit_unusable);
    auto d = deliver(o);
    workdir dir(o.workdir);
    auto line = protocol::wrap(p.cmd, d.command, d.env);
    if (!line)
        throw error("the profile: " + line.error(), exit_unusable);
    if (p.cmd.line_max > 0 && int(line->size()) > p.cmd.line_max) {
        // NOT TYPED: a console's input line is bounded, and a longer one reaches the shell cut
        // (measured: a 600-character line on FreeBSD's serial console). It is written to the files
        // disk and a short line runs it.
        if (p.files_at.empty())
            throw error("the command line is longer than the profile's [command] line-max, and the profile "
                        "states no [files] at to write it to",
                        exit_unusable);
        const auto file = dir.path() / "dirun-command.sh";
        std::ofstream(file, std::ios::binary) << *line << "\n";
        d.files.push_back({str(file), "dirun-command.sh"});
        auto short_line = profile::expand(p.cmd.script, {{"script", p.files_at + "/dirun-command.sh"}});
        if (!short_line)
            throw error("the profile: [command] script: " + short_line.error(), exit_unusable);
        line = *short_line;
    }
    const auto format = probe_format(image, t);
    const auto overlay = dir.path() / "overlay.qcow2";
    if (auto r = t.run({"qemu-img", "create", "-q", "-f", "qcow2", "-b",
                        str(std::filesystem::absolute(os::fs::from_utf8(image))), "-F", format, str(overlay)});
        r.status != 0)
        throw error("the overlay could not be made: " + r.err, exit_unusable, "check the image with `qemu-img info <image>`");
    std::optional<std::string> disk;
    if (!d.files.empty())
        disk = files_dir(d.files, dir.path());
    const auto arch = o.arch.value_or(p.vm.arch);
    auto argv = qemu_words(o, arch);
    argv.insert(argv.end(), {"-drive", "file=" + str(overlay) + ",if=virtio,format=qcow2"});
    if (disk)
        argv.insert(argv.end(), {"-drive", "file=fat:" + *disk + ",if=virtio,format=raw,readonly=on"});

    auto s = e2e::session::start({.argv = argv});
    if (!s.started())
        throw error(std::format("{} did not start: {}", argv.front(), s.transcript()), exit_unusable,
                    "install qemu (it provides " + argv.front() + ")");
    tracer trace{o.trace, s, argv};
    for (std::size_t i = 0; i < p.boot.size(); ++i) {
        const auto& st = p.boot[i];
        const bool lookahead = st.optional && i + 1 < p.boot.size();
        auto [_, which] = await(s, st.expect, lookahead ? p.boot[i + 1].expect : std::string(), st.timeout, c,
                                "while booting");
        if (which == 2) {  // the next step's pattern came first: this one is passed over
            ++i;
            if (p.boot[i].send)
                say(s, *p.boot[i].send, p.boot[i].line, p.boot[i].pace);
            continue;
        }
        if (st.send)
            say(s, *st.send, st.line, st.pace);
    }
    // The shell's prompt is what the last step awaited; each prepared line and mount ends at one.
    const auto prompt = p.boot.empty() ? std::string() : p.boot.back().expect;
    std::vector<std::string> before = p.cmd.prepare;
    if (disk)
        before.insert(before.end(), p.mount.begin(), p.mount.end());
    for (const auto& b : before) {
        s.send_line(expanded(b, o.vars));
        if (!prompt.empty())
            (void)await(s, prompt, {}, 60, c, "after a prepared line");
    }
    s.send_line(*line);
    auto [text, _] = await(s, p.cmd.status, {}, c.left(), c, "while the command ran");
    auto split = protocol::split(text, p.cmd);
    if (d.report) {
        // The events the program wrote in the guest, read back as a second command's output and
        // appended to the runner's report: the program spoke the protocol as it would here.
        auto words = p.events->read;
        words.push_back(p.events->report);
        const auto read = protocol::wrap(p.cmd, words, {});
        if (!read)
            throw error("the profile: " + read.error(), exit_unusable);
        s.send_line(*read);
        auto [events, _] = await(s, p.cmd.status, {}, c.left(), c, "while the test protocol's events were read");
        std::ofstream out(os::fs::from_utf8(*d.report), std::ios::app | std::ios::binary);
        const auto read_back = protocol::split(events, p.cmd);
        std::string_view rest = read_back.output;
        while (!rest.empty()) {
            const auto nl = rest.find('\n');
            const auto event = rest.substr(0, nl);
            if (event.starts_with('{'))
                out << event << '\n';
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        }
    }
    return {split.status & 0xFF, split.output};
}

result run_payload(const options& o, const clock& c) {
    const auto& p = o.prof;
    if (!p.boot_payload)
        throw error("the profile " + p.origin + " has no [payload]: it boots no payload", exit_unusable);
    const auto& pl = *p.boot_payload;
    if (std::error_code ec; !std::filesystem::exists(os::fs::from_utf8(o.payload), ec))
        throw error("the payload does not exist: " + o.payload, exit_unusable);
    workdir dir(o.workdir);
    const auto root = dir.path() / "payload";
    auto vars = o.vars;
    vars["payload"] = str(std::filesystem::absolute(os::fs::from_utf8(o.payload)));
    vars["dir"] = str(root);
    {
        std::string args;  // the program's arguments, as a launcher file writes them
        for (const auto& a : o.command)
            args += (args.empty() ? "" : " ") + a;
        vars["args"] = args;
    }
    std::filesystem::create_directories(root);
    auto put = [&](const std::filesystem::path& from, const std::string& name) {
        const auto to = root / os::fs::from_utf8(name);
        std::error_code ec;
        std::filesystem::create_directories(to.parent_path());
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            throw error("the file to deliver could not be copied: " + str(from) + " (" + ec.message() + ")", exit_unusable);
    };
    if (!pl.place.empty())
        put(os::fs::from_utf8(o.payload), pl.place);
    for (const auto& f : o.files)
        put(os::fs::from_utf8(f.host), f.guest);
    for (const auto& f : pl.beside)
        put(os::fs::from_utf8(f), os::fs::utf8(os::fs::from_utf8(f).filename()));
    for (const auto& [name, text] : pl.files)
        std::ofstream(root / os::fs::from_utf8(name), std::ios::binary) << expanded(text, vars);
    auto opts = o;
    opts.vars = vars;
    if (opts.emulator.empty() && !pl.emulator.empty())
        opts.emulator = {pl.emulator};
    auto argv = pl.machine ? qemu_words(opts, o.arch.value_or(p.vm.arch))
                           : (opts.emulator.empty()
                                  ? std::vector<std::string>{"qemu-system-" + o.arch.value_or(p.vm.arch)}
                                  : opts.emulator);
    for (const auto& a : pl.args)
        argv.push_back(expanded(a, vars));
    e2e::command started{.argv = argv};
    for (const auto& [name, value] : pl.env)
        started.env.emplace_back(name, value);
    auto s = e2e::session::start(started);
    if (!s.started())
        throw error(std::format("{} did not start: {}", argv.front(), s.transcript()), exit_unusable,
                    "install qemu (it provides " + argv.front() + ")");
    tracer trace{o.trace, s, argv};
    s.close_input();
    int status = -1;
    std::string out;
    if (pl.status == "marker") {
        auto [text, _] = await(s, pl.marker, {}, c.left(), c, "while the payload ran");
        std::smatch m;
        const std::regex rx(pl.marker, std::regex::ECMAScript);
        std::regex_search(text, m, rx);
        status = m.size() > 1 && m[1].matched ? std::stoi(m[1].str()) : 0;
        out = text.substr(0, std::size_t(m.position(0)));
    } else {
        while (!s.ended()) {
            if (g_signal)
                throw interrupted{int(g_signal)};
            if (c.left() <= 0)
                throw error("the time limit expired while the payload ran", exit_timeout, "raise --timeout");
            (void)s.expect("$^.", std::chrono::milliseconds{250});  // reads until the end
        }
        auto r = s.finish(std::chrono::seconds{10});
        out = r.out;
        // The transcript marks what was sent; a payload is sent nothing.
        status = pl.status == "debug-exit" ? (r.status - 1) / 2 : r.status;
        if (pl.status == "file") {
            // An emulator whose own status is not the program's (a DOS machine): the launcher
            // wrote the program's in a file of {dir}.
            std::ifstream in(root / os::fs::from_utf8(pl.status_file));
            int written = -1;
            if (!(in >> written))
                throw error(std::format("the payload wrote no status in {}; the emulator wrote:\n{}", pl.status_file,
                                        out.substr(out.size() > 800 ? out.size() - 800 : 0)),
                            exit_no_console, "check the emulator's words and the profile's launcher");
            status = written;
        }
    }
    if (!pl.output_file.empty()) {
        std::ifstream in(root / os::fs::from_utf8(pl.output_file), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        out = text.str();
    }
    out = protocol::normalize(out);
    if (!pl.after.empty()) {
        // THE END OF THE BOOT, AND THE PROOF THAT THERE WAS ONE. An emulator that could not start
        // ends with a status of its own, which under `debug-exit' reads as the payload's 0 (the
        // device writes 2s + 1, and an emulator's failure is 1): measured, a refused drive
        // option was reported as success. A payload whose boot noise ends at a pattern has not
        // run where the pattern never came.
        std::smatch m;
        const std::regex rx(pl.after, std::regex::ECMAScript);
        if (!std::regex_search(out, m, rx))
            throw error(std::format("the console never showed /{}/: the payload did not start; the emulator "
                                    "wrote:\n{}", pl.after, out.substr(out.size() > 800 ? out.size() - 800 : 0)),
                        exit_no_console, "check the emulator's words and the firmware's path");
        out = out.substr(std::size_t(m.position(0) + m.length(0)));
    }
    return {status & 0xFF, out};
}

}  // namespace

std::vector<std::string> qemu_words(const options& o, const std::string& arch) {
    const auto& vm = o.prof.vm;
    std::vector<std::string> argv = o.emulator.empty() ? std::vector<std::string>{"qemu-system-" + arch} : o.emulator;
    // -nographic: the serial line on stdio, and SeaBIOS mirrors the VGA text console onto it, where a
    // loader that writes to the screen is then read.
    argv.insert(argv.end(), {"-nographic", "-no-reboot"});
    if (auto t = o.machine.value_or(vm.type); !t.empty())
        argv.insert(argv.end(), {"-machine", t});
    if (auto cpu = o.cpu.value_or(vm.cpu); !cpu.empty())
        argv.insert(argv.end(), {"-cpu", cpu});
    argv.insert(argv.end(), {"-m", std::to_string(o.memory.value_or(vm.memory)), "-smp",
                             std::to_string(o.smp.value_or(vm.smp))});
    for (const auto& w : accel_words(o.acceleration))
        argv.push_back(w);
    for (const auto& a : vm.args)
        argv.push_back(expanded(a, o.vars));
    return argv;
}

result run(const options& o) {
    system_tools sys;
    tools& t = o.with_tools ? *o.with_tools : sys;
    signals guard;
    const clock c{.limit = o.timeout};
    try {
        return o.payload.empty() ? run_guest(o, t, c) : run_payload(o, c);
    } catch (const interrupted& i) {
        return {128 + i.signal, {}, i.signal};
    }
}

}  // namespace dirun::runner
