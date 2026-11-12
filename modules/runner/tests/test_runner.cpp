// The runner's semantics, one case per behaviour the frozen Python suite pinned
// (tools/reference/tests/test_runner.py, test_hardening.py). Tools and the qemu
// channel answer through the runner's two test seams, so no qemu is needed.
import std;
import dirun.console;
import dirun.runner;
import dirun.testing;

namespace r = dirun::runner;
namespace c = dirun::console;

namespace {

// Answers for the system's tools: what to say each program is, and what each
// invocation returns.
class FakeTools final : public r::ToolRunner {
public:
    std::set<std::string> present;
    std::map<std::string, r::ToolResult> by_program;
    std::vector<std::vector<std::string>> calls;

    r::ToolResult run(const std::vector<std::string>& argv) override {
        calls.push_back(argv);
        const auto found = by_program.find(argv.front());
        if (found != by_program.end()) return found->second;
        return {0, "", ""};  // an unscripted program succeeds quietly
    }

    std::optional<std::string> find(const std::string& name) override {
        if (present.contains(name)) return "/fakebin/" + name;
        return std::nullopt;
    }
};

// A channel that answers from a script or throws: the broken console's face.
class ScriptedChannel final : public c::Channel {
public:
    std::function<std::string(const std::string&)> on_read;
    std::vector<std::string> sent;

    std::string read_until(const std::string& pattern, double) override {
        if (!on_read) throw c::ConsoleError{"the channel is broken"};
        return on_read(pattern);
    }

    void send(const std::string& line, bool) override { sent.push_back(line); }
};

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "dirun-runner-test" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

int main() {
    using namespace dirun::testing;

    "the architecture comes from the machine, x86_64 otherwise"_test = [] {
        expect(r::detect_arch(std::nullopt) == "x86_64");
        const std::pair<const char*, const char*> table[] = {
            {"q35", "x86_64"}, {"pc", "x86_64"}, {"microvm", "x86_64"},
            {"virt", "aarch64"}, {"vexpress-a9", "arm"}, {"virt,accel=kvm", "aarch64"},
            {"something-else", "x86_64"},
        };
        for (const auto& [machine, arch] : table)
            expect(r::detect_arch(machine) == arch) << machine;
    };

    "the qemu line boots the overlay and exposes the file disk read-only"_test = [] {
        r::RunOptions options;
        options.memory_mib = 1024;
        options.smp = 4;
        options.machine = "virt";
        options.cpu = "max";
        options.acceleration = r::accel::tcg;
        const auto argv = r::build_qemu_argv("/o.qcow2", "aarch64", options, "/f.vfat");
        expect(argv.front() == "qemu-system-aarch64");
        const auto has = [&](const std::string& flag, const std::string& value) {
            for (auto at = argv.begin(); (at = std::find(at, argv.end(), flag)) != argv.end(); ++at)
                if (at + 1 != argv.end() && *(at + 1) == value) return true;  // any occurrence
            return false;
        };
        std::string joined;
        for (const auto& word : argv) joined += word + " ";
        expect(has("-cpu", "max")) << joined;
        expect(has("-m", "1024")) << joined;
        expect(has("-smp", "4")) << joined;
        expect(has("-accel", "tcg")) << joined;
        expect(has("-drive", "file=/o.qcow2,if=virtio")) << joined;
        expect(has("-drive", "file=/f.vfat,format=raw,if=virtio,readonly=on")) << joined;

        const auto plain = r::build_qemu_argv("/o", "x86_64", r::RunOptions{}, std::nullopt);
        std::string plain_joined;
        for (const auto& word : plain) plain_joined += word + " ";
        expect(std::none_of(plain.begin(), plain.end(),
                            [](const std::string& word) {
                                return word.find("readonly=on") != std::string::npos;
                            })) << plain_joined;
        expect(std::find(plain.begin(), plain.end(), "kvm:tcg") != plain.end()) << plain_joined;
    };

    "an image that is not there is 126 with its remedy"_test = [] {
        try {
            r::RunOptions options;
            options.image = "/nonexistent/img.qcow2";
            options.command = {"true"};
            (void)r::run(options);
            expect(false) << "must throw";
        } catch (const r::RunnerError& error) {
            expect(error.exit_code() == 126);
            expect(error.remedy().find("--image") != std::string::npos);
        }
    };

    "qemu absent from PATH is 126 with its remedy"_test = [] {
        const auto dir = scratch("no-qemu");
        const auto image = dir / "img.qcow2";
        std::ofstream{image} << ' ';
        FakeTools tools;  // nothing present
        try {
            r::RunOptions options;
            options.image = image.string();
            options.command = {"true"};
            options.tools = &tools;
            (void)r::run(options);
            expect(false) << "must throw";
        } catch (const r::RunnerError& error) {
            expect(error.exit_code() == 126);
            expect(error.message().find("install qemu") != std::string::npos);
        }
        std::filesystem::remove_all(dir);
    };

    "the overlay chains onto the base image, which is never booted"_test = [] {
        const auto dir = scratch("overlay");
        const auto image = dir / "img.qcow2";
        std::ofstream{image} << ' ';
        FakeTools tools;
        tools.present = {"qemu-img", "qemu-system-x86_64"};
        tools.by_program["qemu-img"] = {0, R"({"format": "qcow2"})", ""};
        r::RunOptions options;
        options.image = image.string();
        options.command = {"true"};
        options.tools = &tools;
        options.workdir = (dir / "wd").string();
        options.channel_factory = [](const std::vector<std::string>&) ->
            std::unique_ptr<c::Channel> { throw c::ConsoleError{"stop here"}; };
        try { (void)r::run(options); } catch (const r::RunnerError&) {}
        bool saw_backing = false;
        for (const auto& call : tools.calls) {
            if (call.front() == "qemu-img" && call[1] == "create") {
                saw_backing = std::find(call.begin(), call.end(), "-b") != call.end()
                              && std::find(call.begin(), call.end(), image.string()) != call.end();
            }
        }
        expect(saw_backing);
        expect(!std::filesystem::exists(dir / "wd")) << "the work directory must be cleaned";
        std::filesystem::remove_all(dir);
    };

    "the mtools path builds the volume and copies each file"_test = [] {
        const auto dir = scratch("mtools");
        const auto host = dir / "f.txt";
        std::ofstream{host} << "payload";
        FakeTools tools;
        tools.present = {"mkfs.vfat", "mcopy"};
        const auto volume = r::build_files_disk({{host.string(), "/g/f.txt"}}, dir.string(), tools);
        std::string trace = volume + " | ";
        for (const auto& call : tools.calls) {
            for (const auto& word : call) trace += word + " ";
            trace += "| ";
        }
        expect(volume.ends_with("files.vfat")) << trace;
        expect(tools.calls.size() == 2) << trace;
        expect(tools.calls[0].front() == "mkfs.vfat");
        expect(tools.calls[1].front() == "mcopy");
        expect(tools.calls[1].back() == "::/g/f.txt");  // the leading slash is stripped
        std::filesystem::remove_all(dir);
    };

    "the hdiutil path creates, attaches, copies and detaches"_test = [] {
        const auto dir = scratch("hdiutil");
        const auto host = dir / "f.txt";
        std::ofstream{host} << "payload";
        FakeTools tools;
        tools.present = {"hdiutil"};
        // create writes the volume, attach writes the mountpoint's marker file.
        tools.by_program["hdiutil"] = {0, "", ""};
        const auto volume = r::build_files_disk({{host.string(), "/d/f.txt"}}, dir.string(), tools);
        expect(volume.find("files.vfat") != std::string::npos);
        std::vector<std::string> verbs;
        for (const auto& call : tools.calls) verbs.push_back(call[1]);
        expect(verbs == std::vector<std::string>{"create", "attach", "detach"});
        expect(tools.calls[0][3] == "MS-DOS FAT32");
        std::filesystem::remove_all(dir);
    };

    "no FAT tooling at all is 126 naming both remedies"_test = [] {
        const auto dir = scratch("no-fat-tools");
        const auto host = dir / "f.txt";
        std::ofstream{host} << "x";
        FakeTools tools;  // nothing present
        try {
            (void)r::build_files_disk({{host.string(), "/f.txt"}}, dir.string(), tools);
            expect(false) << "must throw";
        } catch (const r::RunnerError& error) {
            expect(error.exit_code() == 126);
            expect(error.remedy().find("brew install mtools") != std::string::npos);
            expect(error.remedy().find("hdiutil") != std::string::npos);
        }
        std::filesystem::remove_all(dir);
    };

    "a broken console is 125 pointing at the expect script"_test = [] {
        const auto dir = scratch("broken-console");
        const auto image = dir / "img.qcow2";
        std::ofstream{image} << ' ';
        FakeTools tools;
        tools.present = {"qemu-img", "qemu-system-x86_64"};
        tools.by_program["qemu-img"] = {0, R"({"format": "qcow2"})", ""};
        r::RunOptions options;
        options.image = image.string();
        options.command = {"true"};
        options.tools = &tools;
        options.timeout_seconds = 10.0;
        options.channel_factory = [](const std::vector<std::string>&) ->
            std::unique_ptr<c::Channel> { return std::make_unique<ScriptedChannel>(); };
        try {
            (void)r::run(options);
            expect(false) << "must throw";
        } catch (const r::RunnerError& error) {
            expect(error.exit_code() == 125);
            expect(error.message().find("expect pattern") != std::string::npos);
        }
        std::filesystem::remove_all(dir);
    };

    "a spent budget is 124 pointing at --timeout"_test = [] {
        const auto dir = scratch("timeout");
        const auto image = dir / "img.qcow2";
        std::ofstream{image} << ' ';
        FakeTools tools;
        tools.present = {"qemu-img", "qemu-system-x86_64"};
        tools.by_program["qemu-img"] = {0, R"({"format": "qcow2"})", ""};
        r::RunOptions options;
        options.image = image.string();
        options.command = {"true"};
        options.tools = &tools;
        options.timeout_seconds = 0.0;  // nothing left before boot even begins
        try {
            (void)r::run(options);
            expect(false) << "must throw";
        } catch (const r::RunnerError& error) {
            expect(error.exit_code() == 124);
            expect(error.remedy().find("--timeout") != std::string::npos);
        }
        std::filesystem::remove_all(dir);
    };

    "an interruption passes through for the CLI to map (130/143)"_test = [] {
        const auto dir = scratch("interrupted");
        const auto image = dir / "img.qcow2";
        std::ofstream{image} << ' ';
        FakeTools tools;
        tools.present = {"qemu-img", "qemu-system-x86_64"};
        tools.by_program["qemu-img"] = {0, R"({"format": "qcow2"})", ""};
        for (const int signum : {2, 15}) {
            r::RunOptions options;
            options.image = image.string();
            options.command = {"true"};
            options.tools = &tools;
            options.workdir = (dir / "wd").string();
            options.channel_factory = [signum](const std::vector<std::string>&) ->
                std::unique_ptr<c::Channel> {
                    class Interrupted final : public c::Channel {
                    public:
                        explicit Interrupted(int signum) : signum_(signum) {}
                        std::string read_until(const std::string&, double) override {
                            throw c::ConsoleInterrupted{signum_};
                        }
                        void send(const std::string&, bool) override {}
                    private:
                        int signum_;
                    };
                    return std::make_unique<Interrupted>(signum);
                };
            try {
                (void)r::run(options);
                expect(false) << "must throw";
            } catch (const c::ConsoleInterrupted& interrupt) {
                expect(r::exit_for_signal(interrupt.signum()) == 128 + signum);
            }
            expect(!std::filesystem::exists(dir / "wd")) << "cleanup must run for a signal too";
            std::filesystem::create_directories(dir / "wd");  // reset for the next signum
        }
        std::filesystem::remove_all(dir);
    };

    "a program that cannot start is 126, not a closed console"_test = [] {
        const auto dir = scratch("spawn-fail");
        const auto image = dir / "img.qcow2";
        std::ofstream{image} << ' ';
        FakeTools tools;
        tools.present = {"qemu-img", "qemu-system-x86_64"};
        tools.by_program["qemu-img"] = {0, R"({"format": "qcow2"})", ""};
        r::RunOptions options;
        options.image = image.string();
        options.command = {"true"};
        options.tools = &tools;
        options.channel_factory = [](const std::vector<std::string>&) ->
            std::unique_ptr<c::Channel> { throw c::spawn_error{"cannot start 'qemu-system-x86_64'"}; };
        try {
            (void)r::run(options);
            expect(false) << "must throw";
        } catch (const r::RunnerError& error) {
            expect(error.exit_code() == 126);
            expect(error.message().find("cannot start") != std::string::npos);
        }
        std::filesystem::remove_all(dir);
    };

    "a run through the markers returns the program's rc and stdout"_test = [] {
        const auto dir = scratch("happy");
        const auto image = dir / "img.qcow2";
        std::ofstream{image} << ' ';
        FakeTools tools;
        tools.present = {"qemu-img", "qemu-system-x86_64"};
        tools.by_program["qemu-img"] = {0, R"({"format": "qcow2"})", ""};
        auto channel = std::make_shared<ScriptedChannel>();
        channel->on_read = [](const std::string& pattern) -> std::string {
            static int step = 0;
            if (pattern.find("@@BEGIN@@") != std::string::npos) { step = 1; return "echo @@BEGIN@@"; }
            if (pattern.find("@@STATUS=") != std::string::npos) {
                ++step;
                return "\r\nhello\r\n@@STATUS=5@@\r\n";
            }
            return "# ";
        };
        r::RunOptions options;
        options.image = image.string();
        options.command = {"./app"};
        options.tools = &tools;
        options.workdir = (dir / "wd").string();
        options.channel_factory = [&](const std::vector<std::string>&) ->
            std::unique_ptr<c::Channel> {
                return std::make_unique<ScriptedChannel>(*channel);
            };
        const auto result = r::run(options);
        expect(result.exit_code == 5);
        expect(result.output == "hello");
        expect(!std::filesystem::exists(dir / "wd"));
        std::filesystem::remove_all(dir);
    };

    return report();
}
