// The runner: QEMU's command line, the image booted in snapshot mode, the files disk, the guest,
// payload and reached modes, and the exit codes (docs/contract.md):
//
//     0-255  the program's own status (guest), or the payload's (payload), passed through
//     124    the run's time limit expired first
//     125    the console ended, or a marker never came
//     126    a prerequisite is unusable (the emulator, the image, the profile, a file to deliver)
//     130    interrupted (SIGINT); 143 terminated (SIGTERM); the emulator is ended either way
module;
#include <csignal>
export module dirun.runner;

import std;
import mcpplibs.os;
import mcpplibs.mtest.e2e;
import dirun.fat;
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
    // The files the program reads from its own directory (MCPP_RUNTIME_FILES of the build tool that
    // started this one, 0.4.0): `guest' is the name relative to the program's directory.
    std::vector<file_delivery> runtime_files;
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
    // that installs its emulator names it). Test seam: a fixed work directory (removed all the
    // same).
    std::vector<std::string> emulator;
    std::optional<std::filesystem::path> workdir;
};

struct result {
    int status = 0;
    std::string output;
    std::string error_output;  // the program's standard error, where the guest keeps it apart (0.4.0)
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

// The runtime files beside a program whose guest name is `program': each destination is relative
// to the program's directory. One that climbs above it has no place where the program was put, and
// is reported rather than dropped in silence.
std::vector<file_delivery> beside_program(const options& o, const std::string& program) {
    std::vector<file_delivery> out;
    const auto slash = program.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : program.substr(0, slash + 1);
    for (const auto& f : o.runtime_files) {
        if (f.guest.starts_with("../") || f.guest.starts_with('/')) {
            std::println(std::cerr, "dirun: warning: {} is not delivered: its place ({}) is outside the "
                                    "program's directory", f.host, f.guest);
            continue;
        }
        out.push_back({f.host, dir + f.guest});
    }
    return out;
}

std::string expanded(const std::string& text, const std::map<std::string, std::string>& vars) {
    auto e = profile::expand(text, vars);
    if (!e)
        throw error("the profile: " + e.error(), exit_unusable);
    return *e;
}

// THE FILES AS A TAR ARCHIVE ON A DISK OF ITS OWN (`[files] form = "tar"', 0.7.0): for a guest that
// reads no FAT volume, or reads one unreliably (measured: GNU/Hurd's fatfs does not answer on a
// FAT32 volume). The archive is the disk, from its first sector: `tar xf <device>' is the mount
// step. Plain ustar: directories and regular files, executable, under their relative names.
// BEHIND THE SYSTEM'S OWN LABEL (0.8.0): with `[files] label', the disk begins with the label's
// bytes and the archive begins at `[files] offset'; with `[files] size' the disk has that size, and
// files that do not fit it are refused before the machine starts.
void tar_field(std::array<char, 512>& h, std::size_t at, std::size_t width, std::uint64_t value) {
    const auto text = std::format("{:0{}o}", value, width - 1);
    std::copy(text.begin(), text.end(), h.begin() + static_cast<std::ptrdiff_t>(at));
}
void zeros(std::ofstream& out, std::uint64_t n) {
    static const std::string block(1u << 16, '\0');
    for (; n >= block.size(); n -= block.size())
        out.write(block.data(), static_cast<std::streamsize>(block.size()));
    out.write(block.data(), static_cast<std::streamsize>(n));
}
void write_tar(const std::filesystem::path& root, const std::filesystem::path& image, const profile::profile& p) {
    std::ofstream out(image, std::ios::binary);
    std::uint64_t written = 0;  // the disk's bytes so far: the label and its gap, then the archive
    if (!p.files_label.empty()) {
        std::ifstream in(os::fs::from_utf8(p.files_label), std::ios::binary);
        if (!in)
            throw error("the files disk's label could not be read: " + p.files_label, exit_unusable,
                        "install the image's package again: the label ships beside its profile");
        const std::string label{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (label.size() > p.files_offset)
            throw error(std::format("the files disk's label ({} bytes) is longer than [files] offset ({})",
                                    label.size(), p.files_offset), exit_unusable);
        out.write(label.data(), static_cast<std::streamsize>(label.size()));
        written = label.size();
    }
    zeros(out, p.files_offset - written);
    written = p.files_offset;
    std::vector<std::filesystem::path> entries;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
        entries.push_back(e.path());
    std::ranges::sort(entries);
    for (const auto& path : entries) {
        const bool is_dir = std::filesystem::is_directory(path);
        auto name = os::fs::utf8(std::filesystem::relative(path, root));
        std::ranges::replace(name, '\\', '/');
        if (is_dir) name += '/';
        std::string prefix;
        if (name.size() > 100) {
            const auto cut = name.rfind('/', 155);
            if (cut == std::string::npos || name.size() - cut - 1 > 100 || cut > 155)
                throw error("a delivered file's name is too long for the files archive: " + name, exit_unusable);
            prefix = name.substr(0, cut);
            name = name.substr(cut + 1);
        }
        const std::uint64_t size = is_dir ? 0 : std::filesystem::file_size(path);
        std::array<char, 512> h{};
        std::copy(name.begin(), name.end(), h.begin());
        tar_field(h, 100, 8, 0755);
        tar_field(h, 108, 8, 0);
        tar_field(h, 116, 8, 0);
        tar_field(h, 124, 12, size);
        tar_field(h, 136, 12, 0);
        std::fill(h.begin() + 148, h.begin() + 156, ' ');
        h[156] = is_dir ? '5' : '0';
        std::copy_n("ustar", 6, h.begin() + 257);
        h[263] = '0'; h[264] = '0';
        std::copy(prefix.begin(), prefix.end(), h.begin() + 345);
        unsigned sum = 0;
        for (const char c : h) sum += static_cast<unsigned char>(c);
        const auto text = std::format("{:06o}", sum);
        std::copy(text.begin(), text.end(), h.begin() + 148);
        h[154] = 0; h[155] = ' ';
        out.write(h.data(), 512);
        written += 512;
        if (!is_dir) {
            std::ifstream in(path, std::ios::binary);
            out << in.rdbuf();
            const std::uint64_t pad = (512 - size % 512) % 512;
            out.write(std::string(pad, '\0').data(), static_cast<std::streamsize>(pad));
            written += size + pad;
        }
    }
    // The end of the archive, and a disk of whole mebibytes: a device has a size its driver accepts.
    // A stated size is the label's: the disk has it, and an archive that does not fit is refused.
    std::uint64_t whole = ((written + 1024) / (1u << 20) + 1) * (1u << 20);
    if (p.files_size != 0) {
        if (written + 1024 > p.files_size)
            throw error(std::format("the delivered files take {} bytes on the files disk, which holds {} from its "
                                    "[files] offset ({}) to its size ({})", written + 1024 - p.files_offset,
                                    p.files_size - p.files_offset, p.files_offset, p.files_size),
                        exit_unusable, "deliver fewer or smaller files");
        whole = p.files_size;
    }
    zeros(out, whole - written);
    if (!out)
        throw error("the files archive could not be written: " + str(image), exit_unusable);
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

// `at': where the guest finds the delivered files ([files] at for a console's guest, [reach] at
// for a reached one).
delivery deliver_to(const options& o, const std::string& at) {
    const auto& p = o.prof;
    delivery d{o.command, o.env, o.files, std::nullopt};
    auto offered = [&](const std::string& host, const std::string& name) {
        if (at.empty())
            throw error("the profile " + p.origin + " states no place for files ([files] at, [reach] at): a "
                        "file of this machine has no place in the guest", exit_unusable);
        d.files.push_back({host, name});
        return at + "/" + name;
    };
    // A program offered from here is run from its copy; a name that is no file here is the guest's.
    if (std::error_code ec; !d.command.empty() &&
                            std::filesystem::is_regular_file(os::fs::from_utf8(d.command.front()), ec)) {
        const auto name = os::fs::utf8(os::fs::from_utf8(d.command.front()).filename());
        d.command.front() = offered(d.command.front(), name);
        for (const auto& f : beside_program(o, name))
            (void)offered(f.host, f.guest);
    }
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

result run_guest(const options& o, const clock& c) {
    const auto& p = o.prof;
    const auto image = o.image.empty() ? p.vm.image : o.image;
    if (image.empty())
        throw error("no image: name one with --image, or a profile whose [machine] states its image", exit_unusable);
    if (std::error_code ec; !std::filesystem::exists(os::fs::from_utf8(image), ec))
        throw error("the image does not exist: " + image, exit_unusable, "pass an existing disk image to --image");
    if (p.cmd.line.empty())
        throw error("the profile " + p.origin + " has no [command]: it cannot run a command", exit_unusable);
    auto d = deliver_to(o, p.files_at);
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
    std::optional<std::string> disk;
    if (!d.files.empty())
        disk = files_dir(d.files, dir.path());
    const auto arch = o.arch.value_or(p.vm.arch);
    auto argv = qemu_words(o, arch);
    // NEVER WRITTEN: the emulator keeps the guest's writes in a temporary overlay of its own
    // (`snapshot=on'), which it discards when it ends, and reads the image's format itself unless
    // the profile states it. No tool of the host's makes an overlay.
    // THE BUS IS THE PROFILE'S (0.7.0): a system with no virtio driver --- one whose disks are
    // behind a controller it emulates, IDE or AHCI by the machine type --- finds neither disk on
    // virtio (measured: GNU/Hurd, `ext2fs: device:wd0: No such device or address').
    std::string drive = "file=" + str(std::filesystem::absolute(os::fs::from_utf8(image))) + ",if=" + p.vm.bus + ",snapshot=on";
    if (!p.vm.format.empty())
        drive += ",format=" + p.vm.format;
    argv.insert(argv.end(), {"-drive", drive});
    if (disk && p.files_form == "tar") {
        const auto archive = dir.path() / "files.tar";
        write_tar(os::fs::from_utf8(*disk), archive, p);
        argv.insert(argv.end(), {"-drive", "file=" + str(archive) + ",if=" + p.vm.bus + ",format=raw,snapshot=on"});
    } else if (disk && p.vm.bus == "virtio") {
        argv.insert(argv.end(), {"-drive", "file=fat:" + *disk + ",if=virtio,format=raw,readonly=on"});
    } else if (disk) {
        // An emulated IDE disk cannot be read-only (QEMU: `Block node is read-only'), and a
        // directory presented as a FAT volume is. The files are written into a disk image of
        // their own (dirun.fat) and the guest is given that, its writes kept in the overlay.
        const auto files_image = dir.path() / "files.img";
        if (auto w = fat::write(os::fs::from_utf8(*disk), files_image); !w)
            throw error("the files' FAT image could not be written: " + w.error(), exit_unusable);
        argv.insert(argv.end(), {"-drive", "file=" + str(files_image) + ",if=" + p.vm.bus + ",format=raw,snapshot=on"});
    }

    // The emulator's temporary files --- the snapshot overlay --- are in the run's directory,
    // removed with it; a system's own temporary directory may not exist (measured: no /var/tmp).
    e2e::command started{.argv = argv};
    for (const char* name : {"TMPDIR", "TMP", "TEMP"})
        started.env.emplace_back(name, str(dir.path()));
    auto s = e2e::session::start(started);
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
        std::string args;  // the program's arguments, as the reader of the launcher file takes them
        for (const auto& a : o.command)
            args += (args.empty() ? "" : " ") + protocol::quote(pl.quote, a);
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
    if (!pl.place.empty())
        for (const auto& f : beside_program(o, pl.place))
            put(os::fs::from_utf8(f.host), f.guest);
    else if (!o.runtime_files.empty())
        std::println(std::cerr, "dirun: warning: the profile places no payload in a directory ([payload] "
                                "place), so its {} runtime file(s) are not delivered", o.runtime_files.size());
    for (const auto& f : pl.beside)
        put(os::fs::from_utf8(f), os::fs::utf8(os::fs::from_utf8(f).filename()));
    for (const auto& [name, text] : pl.files)
        std::ofstream(root / os::fs::from_utf8(name), std::ios::binary) << expanded(text, vars);
    if (pl.disk == "fat-image") {
        // The emulator is given a writable disk holding {dir} ({disk}), not the directory: a
        // directory presented as a FAT volume is read-only (QEMU's `fat:' without `rw').
        const auto image = dir.path() / "payload.img";
        if (auto w = fat::write(root, image); !w)
            throw error("the payload's FAT image could not be written: " + w.error(), exit_unusable);
        vars["disk"] = str(image);
    }
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
    std::string err;
    if (!pl.stderr_file.empty()) {
        // An emulator that merges nothing of the program's standard error into its own output (a
        // DOS machine's command interpreter has no `2>' of its own): the launcher wrote it to a file.
        std::ifstream in(root / os::fs::from_utf8(pl.stderr_file), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        err = protocol::normalize(text.str());
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
    return {status & 0xFF, out, err};
}

std::vector<std::string> expand_all(const std::vector<std::string>& words,
                                    const std::map<std::string, std::string>& vars) {
    std::vector<std::string> out;
    for (const auto& w : words)
        out.push_back(expanded(w, vars));
    return out;
}

// A command of the profile, its templates expanded, run to its end within the run's limit.
e2e::outcome command_of(const std::vector<std::string>& words, const std::map<std::string, std::string>& vars,
                        double seconds, const clock& c, const char* what) {
    if (g_signal)
        throw interrupted{int(g_signal)};
    e2e::command cmd;
    cmd.argv = expand_all(words, vars);
    const double bound = std::max(0.0, std::min(seconds, c.left()));
    cmd.timeout = std::chrono::milliseconds{static_cast<long long>(bound * 1000)};
    auto r = e2e::run(cmd);
    if (!r.started)
        throw error(std::format("{} did not start ({}): {}", cmd.argv.front(), what, r.err), exit_unusable,
                    "install the program the profile names, or give its path with --var");
    if (r.timed_out)
        throw error(std::format("the time limit expired {}", what), exit_timeout, "raise --timeout");
    if (g_signal)
        throw interrupted{int(g_signal)};
    return r;
}

// The machine of a reached guest, for the length of the run: started, awaited until it can be
// reached, and ended however the run ends.
class machine_lifecycle {
public:
    machine_lifecycle(const options& o, const clock& c) : o_(o) {
        if (!o.prof.lifecycle)
            return;
        const auto& st = *o.prof.lifecycle;
        if (!st.command.empty()) {
            // Its outputs go to the null device: nothing reads them for the length of the run, and
            // a pipe nobody reads would stop the machine once it filled.
            os::process::spawn_options so;
            const auto words = expand_all(st.command, o.vars);
            so.program = os::fs::from_utf8(words.front());
            so.args.assign(words.begin() + 1, words.end());
            so.in = so.out = so.err = os::process::stdio::null;
            so.own_unit = true;
            so.bound_lifetime = true;
            auto started = os::process::process::spawn(so);
            if (!started)
                throw error(std::format("{} did not start: {}", words.front(), started.error().message()),
                            exit_unusable, "install the machine the profile's [start] command names");
            machine_ = std::move(*started);
        }
        if (st.ready.empty())
            return;
        // UNTIL IT SUCCEEDS: a device bridge answers once the machine has booted, and a machine that
        // ended first never will.
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(st.ready_timeout);
        for (;;) {
            auto r = command_of(st.ready, o.vars, 60, c, "while the machine started");
            if (r.status == 0)
                return;
            if (machine_.valid())
                if (auto ended = machine_.try_wait(); ended && *ended)
                    throw error(std::format("the machine ended with status {} while it started",
                                            (*ended)->shell_code()),
                                exit_no_console, "check the profile's [start] command");
            if (std::chrono::steady_clock::now() >= end || c.left() <= 0)
                throw error(std::format("the machine was not ready within {} seconds", st.ready_timeout),
                            c.left() <= 0 ? exit_timeout : exit_no_console, "raise [start] ready-timeout or --timeout");
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }
    ~machine_lifecycle() {
        // ASKED FIRST, THEN ENDED: a machine's own way down (an emulator's console command) lets it
        // release what it holds; whatever is left of the started program ends with its unit.
        if (o_.prof.lifecycle && !o_.prof.lifecycle->stop.empty()) {
            e2e::command stop;
            for (const auto& w : o_.prof.lifecycle->stop)
                if (auto e = profile::expand(w, o_.vars))
                    stop.argv.push_back(*e);
            stop.timeout = std::chrono::seconds{60};
            (void)e2e::run(stop);
            if (machine_.valid())
                (void)machine_.wait_for(std::chrono::seconds{30});
        }
        if (machine_.valid())
            machine_.end_unit();
    }
    machine_lifecycle(const machine_lifecycle&) = delete;
    machine_lifecycle& operator=(const machine_lifecycle&) = delete;
private:
    const options& o_;
    os::process::process machine_;
};

// A GUEST REACHED BY COMMANDS (0.4.0): the files are delivered by the profile's `push', the
// command line --- the same [command] line a console is given --- by its `shell', and the output
// and status are read from what that command wrote, between the same markers.
result run_reached(const options& o, const clock& c) {
    const auto& p = o.prof;
    const auto& re = *p.reached;
    machine_lifecycle machine(o, c);
    auto d = deliver_to(o, re.at);
    auto vars = o.vars;
    for (const auto& prep : p.cmd.prepare) {
        vars["line"] = expanded(prep, o.vars);
        (void)command_of(re.shell, vars, 60, c, "while a prepared line ran");
    }
    for (const auto& f : d.files) {
        if (re.push.empty())
            throw error("the profile " + p.origin + " states no [reach] push: a file of this machine has no way "
                        "into the guest", exit_unusable);
        vars["host"] = str(std::filesystem::absolute(os::fs::from_utf8(f.host)));
        vars["guest"] = re.at + "/" + f.guest;
        auto r = command_of(re.push, vars, 300, c, "while a file was delivered");
        if (r.status != 0)
            throw error(std::format("{} was not delivered (status {}): {}{}", f.host, r.status, r.out, r.err),
                        exit_unusable, "check the profile's [reach] push and the guest's `at'");
    }
    auto line = protocol::wrap(p.cmd, d.command, d.env);
    if (!line)
        throw error("the profile: " + line.error(), exit_unusable);
    vars["line"] = *line;
    auto r = command_of(re.shell, vars, c.left(), c, "while the command ran");
    std::smatch m;
    const std::regex status_rx(p.cmd.status, std::regex::ECMAScript);
    if (!std::regex_search(r.out, m, status_rx))
        throw error(std::format("the guest's command wrote no status (the shell's own status {}); it wrote:\n{}{}",
                                r.status, r.out.substr(r.out.size() > 800 ? r.out.size() - 800 : 0), r.err),
                    exit_no_console, "check that the guest can be reached and the profile's [command] line");
    auto split = protocol::split(r.out, p.cmd);
    return {split.status & 0xFF, split.output, protocol::normalize(r.err)};
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
    signals guard;
    const clock c{.limit = o.timeout};
    try {
        if (!o.payload.empty())
            return run_payload(o, c);
        if (o.image.empty() && o.prof.vm.image.empty() && o.prof.reached)
            return run_reached(o, c);
        return run_guest(o, c);
    } catch (const interrupted& i) {
        return {128 + i.signal, {}, {}, i.signal};
    }
}

}  // namespace dirun::runner
