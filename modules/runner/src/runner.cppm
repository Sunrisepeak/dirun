// The runner: qemu invocation, the write-isolated overlay, the file disk, the
// exit codes. The contract's code space, verbatim:
//
//     0-255  the guest program's own rc, passed through
//     124    the overall timeout expired before the program finished
//     125    the console broke, or a marker was never seen
//     126    the runner's prerequisites are unusable
//     130    interrupted (SIGINT) --- the guest's whole process group ended
//     143    terminated (SIGTERM) --- the same cleanup
module;
#include <csignal>
export module dirun.runner;

import std;
import dirun.protocol;
import dirun.console;
import mcpplibs.os;

export namespace dirun::runner {

namespace os = mcpplibs::os;

inline constexpr int exit_timeout = 124;
inline constexpr int exit_no_console = 125;
inline constexpr int exit_runner_unusable = 126;

// What the exit code says when the run was ended by a signal (128 + signum).
inline constexpr int exit_for_signal(int signum) { return 128 + signum; }

class RunnerError : public std::exception {
public:
    RunnerError(std::string cause, int exit_code, std::string remedy)
        : cause_(std::move(cause)), remedy_(std::move(remedy)), exit_code_(exit_code) {}

    int exit_code() const noexcept { return exit_code_; }
    const std::string& cause() const noexcept { return cause_; }
    const std::string& remedy() const noexcept { return remedy_; }

    // The one-line form the CLI prints: a cause sentence, then the remedy.
    std::string message() const {
        return cause_ + (remedy_.empty() ? "" : ". To fix: " + remedy_);
    }
    const char* what() const noexcept override {
        if (formatted_.empty()) formatted_ = message();
        return formatted_.c_str();
    }

private:
    std::string cause_;
    std::string remedy_;
    int exit_code_;
    mutable std::string formatted_;
};

// How the system's tools are run: one seam, so a test answers for qemu-img,
// mkfs.vfat, mcopy and hdiutil without any of them being present.
struct ToolResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

class ToolRunner {
public:
    virtual ~ToolRunner() = default;
    virtual ToolResult run(const std::vector<std::string>& argv) = 0;
    // An empty optional: no such program on PATH.
    virtual std::optional<std::string> find(const std::string& name) = 0;
};

// The real one: the system's own programs, run with their output collected.
class SystemToolRunner final : public ToolRunner {
public:
    ToolResult run(const std::vector<std::string>& argv) override {
        os::process::spawn_options options;
        options.program = argv.front();
        options.args.assign(argv.begin() + 1, argv.end());
        options.in = os::process::stdio::null;
        auto finished = os::process::run(options);
        if (!finished) return {127, "", finished.error().message()};
        ToolResult result;
        result.exit_code = finished->status.shell_code();
        result.out = std::move(finished->out);
        result.err = std::move(finished->err);
        return result;
    }

    std::optional<std::string> find(const std::string& name) override {
        const auto found = os::env::find_executable(name);
        if (!found || found->empty()) return std::nullopt;
        return *found;
    }
};

// Known machine -> qemu system architecture; x86_64 is the default and
// --machine the explicit override.
inline std::string detect_arch(const std::optional<std::string>& machine) {
    static const std::map<std::string, std::string> by_machine{
        {"q35", "x86_64"}, {"pc", "x86_64"}, {"microvm", "x86_64"},
        {"virt", "aarch64"}, {"vexpress-a9", "arm"},
    };
    if (!machine) return "x86_64";
    const auto name = machine->substr(0, machine->find(','));
    const auto found = by_machine.find(name);
    return found != by_machine.end() ? found->second : "x86_64";
}

// The base image's format, so the overlay chain is right.
inline std::string probe_format(const std::string& image, ToolRunner& tools) {
    if (!tools.find("qemu-img"))
        throw RunnerError{"qemu-img not found on PATH", exit_runner_unusable,
                          "install qemu (qemu-img ships with it)"};
    const auto info = tools.run({"qemu-img", "info", "--output=json", image});
    if (info.exit_code != 0)
        throw RunnerError{"qemu-img cannot read " + image + ": " + info.err, exit_runner_unusable,
                          "verify the image path and format with `qemu-img info <image>`"};
    const std::regex format_re{R"re("format"\s*:\s*"([^"]+)")re"};
    std::smatch match;
    if (!std::regex_search(info.out, match, format_re))
        throw RunnerError{"qemu-img did not name a format for " + image, exit_runner_unusable,
                          "verify the image with `qemu-img info <image>`"};
    return match[1].str();
}

struct FileDelivery {
    std::string host;
    std::string guest;
};

// Pack the host files into a read-only FAT second disk. The guest's mount
// commands belong to each system's console script, never here.
inline std::string build_files_disk(const std::vector<FileDelivery>& files,
                                    const std::string& workdir, ToolRunner& tools) {
    std::uint64_t mib = 16;
    for (const auto& delivery : files) {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(delivery.host, size_error);
        if (!size_error) mib += std::max<std::uint64_t>(1, (size + 1024 * 1024 - 1) / (1024 * 1024));
    }

    const auto mtools_remedy =
        "install mtools (e.g. brew install mtools / apt install mtools) "
        "or use the system hdiutil (bundled with macOS)";
    const std::string volume = (std::filesystem::path{workdir} / "files.vfat").string();
    const bool have_mtools = tools.find("mkfs.vfat") && tools.find("mcopy");
    const bool have_hdiutil = static_cast<bool>(tools.find("hdiutil"));

    if (have_mtools) {
        std::filesystem::path volume_path{volume};
        { std::ofstream touch{volume_path}; }  // there, then sized
        std::error_code ignored;
        std::filesystem::resize_file(volume_path, mib * 1024 * 1024, ignored);
        auto mkfs = tools.run({"mkfs.vfat", volume});
        if (mkfs.exit_code != 0)
            throw RunnerError{"mkfs.vfat failed: " + mkfs.err, exit_runner_unusable, mtools_remedy};
        for (const auto& delivery : files) {
            std::string guest = delivery.guest;
            while (!guest.empty() && guest.front() == '/') guest.erase(guest.begin());
            const auto copied = tools.run({"mcopy", "-i", volume, delivery.host, "::/" + guest});
            if (copied.exit_code != 0)
                throw RunnerError{"mcopy " + delivery.host + " -> " + guest + " failed: " + copied.err,
                                  exit_runner_unusable, mtools_remedy};
        }
        return volume;
    }

    if (have_hdiutil) {
        const auto created = tools.run({"hdiutil", "create", "-fs", "MS-DOS FAT32", "-layout", "NONE",
                                        "-size", std::to_string(mib) + "m", "-ov", "-o", volume});
        if (created.exit_code != 0)
            throw RunnerError{"hdiutil create failed: " + created.err, exit_runner_unusable,
                              "hdiutil ships with macOS; check disk space and permissions"};
        // hdiutil may append .dmg when the given name has no image suffix.
        std::filesystem::path image{volume};
        std::error_code absent;
        if (!std::filesystem::exists(image, absent)) image = volume + ".dmg";
        const std::string mountpoint = volume + ".mnt";
        std::filesystem::create_directories(mountpoint);
        auto attached =
            tools.run({"hdiutil", "attach", "-nobrowse", "-mountpoint", mountpoint, image.string()});
        if (attached.exit_code != 0)
            throw RunnerError{"hdiutil attach failed: " + attached.err, exit_runner_unusable,
                              "check that the volume is not already mounted"};
        try {
            for (const auto& delivery : files) {
                std::string guest = delivery.guest;
                while (!guest.empty() && guest.front() == '/') guest.erase(guest.begin());
                const auto target = std::filesystem::path{mountpoint} / guest;
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy_file(delivery.host, target,
                                           std::filesystem::copy_options::overwrite_existing);
            }
        } catch (...) {
            (void)tools.run({"hdiutil", "detach", mountpoint});
            throw;
        }
        (void)tools.run({"hdiutil", "detach", mountpoint});
        return image.string();
    }

    throw RunnerError{"cannot build the FAT file disk: mtools (mkfs.vfat/mcopy) not found and "
                      "hdiutil is unavailable",
                      exit_runner_unusable, mtools_remedy};
}

enum class accel { auto_, kvm, tcg };

struct RunOptions {
    std::string image;
    std::vector<std::string> command;
    std::optional<std::string> machine;
    std::optional<std::string> cpu;
    int memory_mib = 512;
    int smp = 1;
    dirun::runner::accel acceleration = accel::auto_;
    double timeout_seconds = 300.0;
    std::optional<std::string> expect_script;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<FileDelivery> files;
    ToolRunner* tools = nullptr;  // null: the system's own
    // A test seam: answers for the qemu channel; null: the pty driver.
    std::function<std::unique_ptr<dirun::console::Channel>(const std::vector<std::string>&)> channel_factory;
    // A test seam: a fixed work directory (removed the same way); null: a fresh one.
    std::optional<std::string> workdir;
};

struct RunResult {
    int exit_code = 0;
    std::string output;
};

// The qemu command line: the overlay (never the base) as the boot disk, the
// FAT disk read-only as the second one, serial on stdio via -nographic.
inline std::vector<std::string> build_qemu_argv(const std::string& overlay, const std::string& arch,
                                                const RunOptions& options,
                                                const std::optional<std::string>& files_disk) {
    std::vector<std::string> argv{"qemu-system-" + arch, "-nographic", "-no-reboot"};
    if (options.machine) argv.insert(argv.end(), {"-machine", *options.machine});
    if (options.cpu) argv.insert(argv.end(), {"-cpu", *options.cpu});
    argv.insert(argv.end(), {"-m", std::to_string(options.memory_mib)});
    argv.insert(argv.end(), {"-smp", std::to_string(options.smp)});
    switch (options.acceleration) {
        case accel::auto_: argv.insert(argv.end(), {"-accel", "kvm:tcg"}); break;
        case accel::kvm:   argv.insert(argv.end(), {"-accel", "kvm"});     break;
        case accel::tcg:   argv.insert(argv.end(), {"-accel", "tcg"});     break;
    }
    argv.insert(argv.end(), {"-drive", "file=" + overlay + ",if=virtio"});
    if (files_disk)
        argv.insert(argv.end(),
                    {"-drive", "file=" + *files_disk + ",format=raw,if=virtio,readonly=on"});
    return argv;
}

namespace detail {

inline double now_seconds() {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count()) / 1e6;
}

// A work directory that cannot outlive the run, whatever ends it.
class WorkDir {
public:
    explicit WorkDir(const std::optional<std::string>& fixed = std::nullopt) {
        if (fixed) {
            path_ = *fixed;
        } else {
            static std::atomic<unsigned> serial{0};
            std::string who = "0";
            if (const auto self = os::process::self()) who = std::to_string(self->id);
            path_ = std::filesystem::temp_directory_path()
                    / std::format("dirun-{}-{}", who, serial.fetch_add(1));
        }
        std::filesystem::create_directories(path_);
        text_ = path_.string();
    }
    ~WorkDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    WorkDir(const WorkDir&) = delete;
    WorkDir& operator=(const WorkDir&) = delete;

    const std::string& str() const { return text_; }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::string text_;
};

// The signal guard: SIGINT/SIGTERM are remembered, and the console driver's
// probe turns them into an interruption between reads; the handlers are
// restored on the way out, so a caller's own semantics survive the run.
class SignalGuard {
public:
    SignalGuard() {
        previous_int_ = std::signal(SIGINT, &SignalGuard::handle);
        previous_term_ = std::signal(SIGTERM, &SignalGuard::handle);
        dirun::console::PtyDriver::interrupt_probe = [] { return arrived(); };
    }
    ~SignalGuard() {
        dirun::console::PtyDriver::interrupt_probe = nullptr;
        std::signal(SIGINT, previous_int_);
        std::signal(SIGTERM, previous_term_);
    }
    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;

    static int arrived() { return signum_; }

private:
    static void handle(int signum) { signum_ = signum; }

    inline static volatile std::sig_atomic_t signum_ = 0;
    void (*previous_int_)(int) = SIG_DFL;
    void (*previous_term_)(int) = SIG_DFL;
};

// The boot script's steps, each clamped to what is left of the overall budget.
inline dirun::console::ExpectMachine clamp(const dirun::console::ExpectMachine& machine,
                                           double budget) {
    std::vector<dirun::console::ExpectStep> steps = machine.steps();
    for (auto& step : steps)
        step.timeout_seconds = std::min(step.timeout_seconds, std::max(0.1, budget));
    return dirun::console::ExpectMachine{std::move(steps)};
}

}  // namespace detail

// Run one command inside the image's guest, over the console the image gives.
inline RunResult run(const RunOptions& options) {
    SystemToolRunner system_tools;
    ToolRunner& tools = options.tools ? *options.tools : system_tools;

    std::error_code absent;
    if (!std::filesystem::exists(options.image, absent))
        throw RunnerError{"image not found: " + options.image, exit_runner_unusable,
                          "pass an existing disk image via --image"};

    const std::string arch = detect_arch(options.machine);
    const std::string qemu_bin = "qemu-system-" + arch;
    if (!tools.find(qemu_bin))
        throw RunnerError{qemu_bin + " not found on PATH", exit_runner_unusable,
                          "install qemu (provides " + qemu_bin + ")"};

    detail::WorkDir workdir{options.workdir};
    detail::SignalGuard signals;

    const double started = detail::now_seconds();
    auto remaining = [&] { return options.timeout_seconds - (detail::now_seconds() - started); };

    const std::string base_format = probe_format(options.image, tools);
    const auto overlay = (workdir.path() / "overlay.qcow2").string();
    {
        auto created = tools.run({"qemu-img", "create", "-f", "qcow2", "-b",
                                  std::filesystem::absolute(options.image).string(), "-F", base_format,
                                  overlay});
        if (created.exit_code != 0)
            throw RunnerError{"overlay creation failed: " + created.err, exit_runner_unusable,
                              "check the base image format with `qemu-img info <image>`"};
    }

    std::optional<std::string> files_disk;
    if (!options.files.empty())
        files_disk = build_files_disk(options.files, workdir.str(), tools);

    const auto argv = build_qemu_argv(overlay, arch, options, files_disk);

    std::string script_error;
    const auto steps = dirun::console::load_script(options.expect_script, script_error);
    if (!steps)
        throw RunnerError{script_error, exit_runner_unusable,
                          "check the expect script's path and JSON shape"};
    const dirun::console::ExpectMachine machine{*steps};
    const std::string command_line = dirun::protocol::wrap_command(options.command, options.env);

    if (remaining() <= 0)
        throw RunnerError{"timeout expired before boot began", exit_timeout,
                          "increase --timeout to cover slow boots"};

    std::unique_ptr<dirun::console::Channel> channel;
    try {
        channel = options.channel_factory
                      ? options.channel_factory(argv)
                      : std::unique_ptr<dirun::console::Channel>(
                            std::make_unique<dirun::console::PtyDriver>(argv));
    } catch (const dirun::console::spawn_error& error) {
        throw RunnerError{error.what(), exit_runner_unusable,
                          "install qemu and ensure " + qemu_bin + " is executable"};
    } catch (const dirun::console::ConsoleError& error) {
        throw RunnerError{error.what(), exit_no_console, ""};
    }

    try {
        detail::clamp(machine, remaining()).run(*channel);
        if (remaining() <= 0)
            throw RunnerError{"timeout expired during the boot script", exit_timeout,
                              "increase --timeout to cover slow boots"};
        const auto marked = dirun::console::run_marker_command(*channel, command_line, remaining());
        if (detail::SignalGuard::arrived() != 0)
            throw dirun::console::ConsoleInterrupted{detail::SignalGuard::arrived()};
        return {marked.rc & 0xFF, marked.output};
    } catch (const dirun::console::ConsoleInterrupted&) {
        throw;  // the CLI maps 128 + signum
    } catch (const dirun::console::ConsoleError& error) {
        if (detail::SignalGuard::arrived() != 0)
            throw dirun::console::ConsoleInterrupted{detail::SignalGuard::arrived()};
        if (remaining() <= 0)
            throw RunnerError{std::string{"console: "} + error.what(), exit_timeout,
                              "increase --timeout to cover slow boots"};
        throw RunnerError{std::string{"console: "} + error.what(), exit_no_console,
                          "check that the first expect pattern in your --expect script matches "
                          "early boot output"};
    }
}

}  // namespace dirun::runner
