// The runner against a console played by a copy of this program (no emulator needed): the boot
// steps, the command's output and status, and each exit code of the contract (docs/contract.md).
import std;
import mcpplibs.mtest;
import mcpplibs.mtest.e2e;
import dirun.profile;
import dirun.runner;

using namespace mcpplibs::mtest;
namespace runner = dirun::runner;

namespace {

std::vector<std::string> words_of(const std::string& line) {
    std::vector<std::string> out;
    static const std::regex quoted{R"('([^']*)')"};
    for (std::sregex_iterator it(line.begin(), line.end(), quoted), end; it != end; ++it)
        out.push_back((*it)[1].str());
    return out;
}

}  // namespace

// A guest's console: `mode' is the first argument; the emulator's words follow and are ignored.
// It logs in, then answers each marker line by "running" `say WORDS... exit N'.
child guest{"fake-guest", [](std::vector<std::string> args) {
    const std::string mode = args.empty() ? "" : args.front();
    if (mode == "silent") {
        std::this_thread::sleep_for(std::chrono::seconds{30});
        return 0;
    }
    // The files disk, as QEMU's `fat:' driver would present the directory it names.
    std::string disk;
    for (const auto& a : args)
        if (auto at = a.find("file=fat:"); at != std::string::npos) {
            const auto dir = a.substr(at + 9, a.find(',', at) - at - 9);
            for (const auto& e : std::filesystem::directory_iterator(dir))
                disk += " " + e.path().filename().string();
        }
    std::cout << "Booting...\r\nlogin: " << std::flush;
    if (mode == "dies")
        return 0;
    std::string line;
    if (!std::getline(std::cin, line) || line != "root")
        return 3;
    if (mode != "nopassword") {
        std::cout << "Password:" << std::flush;
        if (!std::getline(std::cin, line) || line != "root")
            return 4;
    }
    std::cout << "\r\n# " << std::flush;
    std::string files_dir;
    for (const auto& a : args)
        if (auto at = a.find("file=fat:"); at != std::string::npos)
            files_dir = a.substr(at + 9, a.find(',', at) - at - 9);
    while (std::getline(std::cin, line)) {
        if (line.starts_with("sh /mnt/dirun/") && !files_dir.empty()) {  // a line written as a file
            std::ifstream script(std::filesystem::path(files_dir) / line.substr(14));
            std::getline(script, line);
            disk += " (from a script)";
        }
        if (!line.starts_with("echo @@BEGIN@@;")) {
            std::cout << line << "\r\n# " << std::flush;  // a prepared line: echoed, a prompt
            continue;
        }
        auto w = words_of(line);
        std::string out;
        int status = 0;
        if (!w.empty() && w.front() == "cat") {  // the events the protocol relay reads back
            std::cout << line << "\r\n@@BEGIN@@\r\n{\"event\":\"case\"}\r\nnot an event\r\n{\"event\":\"end\"}"
                      << "\r\n@@STATUS=0@@\r\n# " << std::flush;
            continue;
        }
        for (std::size_t i = 0; i < w.size(); ++i) {
            if (w[i].starts_with("V="))
                out += w[i].substr(2) + " ";
            else if (w[i] == "exit" && i + 1 < w.size())
                status = std::stoi(w[++i]);
            else if (w[i] != "say")
                out += w[i] + " ";
        }
        if (!disk.empty())
            out += "disk:" + disk;
        std::cout << line << "\r\n@@BEGIN@@\r\n" << out << "\r\n@@STATUS=" << status << "@@\r\n# " << std::flush;
    }
    return 0;
}};

// A firmware: boot noise, then the payload's output, then the status as a debug-exit device
// gives it (2 * status + 1).
child firmware{"fake-firmware", [](std::vector<std::string>) {
    std::cout << "firmware noise\r\nBOOTED\r\nhello from the payload\r\n" << std::flush;
    return 2 * 5 + 1;
}};

// An emulator that is not QEMU and whose own status is not the program's (a DOS machine): given
// the directory, it reads the launcher the profile wrote there, "runs" it, and writes the program's
// output and status as files in that directory. It exits 0 whatever the program did.
child dos{"fake-dos", [](std::vector<std::string> args) {
    if (args.size() != 2 || args[0] != "-dir")
        return 9;  // given QEMU's machine words, or not the profile's own
    const std::filesystem::path dir = args[1];
    std::ifstream run(dir / "RUN.BAT");
    std::string launcher((std::istreambuf_iterator<char>(run)), {});
    const bool dpmi = std::filesystem::exists(dir / "CWSDPMI.EXE");
    std::ofstream(dir / "OUT.TXT") << "ran: " << launcher << (dpmi ? " with dpmi" : "") << "\r\n";
    std::ofstream(dir / "RC.TXT") << "42\r\n";
    return 0;
}};

namespace {

struct fake_tools final : runner::tools {
    std::vector<std::vector<std::string>> calls;
    runner::tool_result run(const std::vector<std::string>& argv) override {
        calls.push_back(argv);
        if (argv.size() > 1 && argv[1] == "info")
            return {0, R"({"children":[{"info":{"format":"file"}}],"format":"qcow2"})", ""};
        return {0, "", ""};
    }
    bool has(const std::string&) override { return true; }
};

runner::options guest_options(std::string mode, fake_tools& t) {
    runner::options o;
    o.image = (temp_dir() / "base.qcow2").generic_string();
    std::ofstream(o.image) << "base";
    o.emulator = child_command("fake-guest", std::vector<std::string>{std::move(mode)});
    o.with_tools = &t;
    o.timeout = 30;
    return o;
}

}  // namespace

UTest status{"the command's output and status pass through", [] {
    fake_tools t;
    auto o = guest_options("password", t);
    o.command = {"say", "hello", "exit", "7"};
    o.env = {{"V", "env-value"}};
    auto r = runner::run(o);
    expect(eq(r.status, 7));
    expect(contains(r.output, "hello"));
    expect(contains(r.output, "env-value")) << "the environment reaches the command";
}};

UTest overlay{"the base image is the backing of an overlay, read with its top-level format", [] {
    fake_tools t;
    auto o = guest_options("password", t);
    o.command = {"say", "x"};
    (void)runner::run(o);
    bool created = false;
    for (const auto& c : t.calls)
        if (c.size() > 1 && c[1] == "create") {
            created = true;
            expect(eq(c[std::size_t(std::ranges::find(c, std::string("-F")) - c.begin()) + 1], std::string("qcow2")))
                << "the backing format is the image's, not its file layer's";
        }
    expect(created);
}};

UTest optional_step{"an optional step is passed over when the next prompt comes first", [] {
    fake_tools t;
    auto o = guest_options("nopassword", t);
    o.command = {"say", "ok", "exit", "0"};
    auto r = runner::run(o);
    expect(eq(r.status, 0));
    expect(contains(r.output, "ok"));
}};

UTest codes{"each way a run cannot finish has its code", [] {
    fake_tools t;
    auto timeout = guest_options("silent", t);
    timeout.command = {"x"};
    timeout.timeout = 2;
    try {
        (void)runner::run(timeout);
        fail("no timeout");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_timeout)) << e.what();
    }
    auto dies = guest_options("dies", t);
    dies.command = {"x"};
    try {
        (void)runner::run(dies);
        fail("no console end");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_no_console)) << e.what();
    }
    auto missing = guest_options("password", t);
    missing.command = {"x"};
    missing.image = (temp_dir() / "absent.qcow2").generic_string();
    try {
        (void)runner::run(missing);
        fail("a missing image ran");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_unusable));
    }
    auto noqemu = guest_options("password", t);
    noqemu.command = {"x"};
    noqemu.emulator = {(temp_dir() / "no-such-emulator").generic_string()};
    try {
        (void)runner::run(noqemu);
        fail("a missing emulator ran");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_unusable)) << e.what();
    }
}};

UTest payload{"a payload's status comes back as its profile says", [] {
    runner::options o;
    o.payload = (temp_dir() / "app.efi").generic_string();
    std::ofstream(o.payload) << "MZ";
    auto p = dirun::profile::parse(R"([payload]
args = ["-drive", "format=raw,file=fat:rw:{dir}", "-bios", "{firmware}"]
place = "EFI/BOOT/BOOTX64.EFI"
status = "debug-exit"
after = "BOOTED\n"
)", "uefi.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    o.prof = *p;
    o.vars = {{"firmware", "/fw.fd"}};
    o.emulator = child_command("fake-firmware", std::vector<std::string>{});
    o.timeout = 30;
    auto r = runner::run(o);
    expect(eq(r.status, 5));
    expect(eq(r.output, std::string("hello from the payload\n")));
}};

UTest payload_never_started{"a payload whose boot never ended did not run, whatever the status", [] {
    runner::options o;
    o.payload = (temp_dir() / "app.efi").generic_string();
    std::ofstream(o.payload) << "MZ";
    auto p = dirun::profile::parse("[payload]\nstatus = \"debug-exit\"\nafter = \"NEVER-PRINTED\"\n", "t.toml");
    expect(fatal(p.has_value()));
    o.prof = *p;
    o.emulator = child_command("fake-firmware", std::vector<std::string>{});
    o.timeout = 30;
    try {
        (void)runner::run(o);
        fail("reported a status");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_no_console)) << e.what();
    }
}};

UTest delivered{"a program of this machine is delivered on the files disk and run from its copy", [] {
    fake_tools t;
    auto o = guest_options("password", t);
    const auto program = temp_dir() / "prog";
    std::ofstream(program) << "binary";
    o.command = {program.generic_string(), "arg"};
    auto r = runner::run(o);
    expect(contains(r.output, "/mnt/dirun/prog arg")) << "the guest runs the copy, by the profile's [files] at";
    expect(contains(r.output, "disk: prog")) << "the program is on the read-only FAT disk the emulator was given";
}};

UTest relayed{"a test runner's events written in the guest are appended to its report", [] {
    fake_tools t;
    auto o = guest_options("password", t);
    const auto report = temp_dir() / "report.ndjson";
    std::ofstream(report) << "{\"event\":\"before\"}\n";
    o.protocol = runner::options::test_protocol{"1", report.generic_string(), "run", ""};
    o.command = {"say", "ran", "exit", "0"};
    auto r = runner::run(o);
    expect(contains(r.output, "MTEST_PROTOCOL=1")) << "the command speaks the protocol";
    expect(contains(r.output, "MTEST_REPORT=/tmp/dirun-mtest.ndjson")) << "to the guest's file";
    std::ifstream in(report);
    std::stringstream got;
    got << in.rdbuf();
    expect(eq(got.str(), std::string("{\"event\":\"before\"}\n{\"event\":\"case\"}\n{\"event\":\"end\"}\n")))
        << "events are appended; other lines are not events";
}};

UTest dos_machine{"an emulator that is not QEMU reports through files a launcher in its directory writes", [] {
    runner::options o;
    o.payload = (temp_dir() / "hello.exe").generic_string();
    std::ofstream(o.payload) << "MZ";
    // The profile as a package ships it: the DPMI host beside it, delivered beside the program.
    const auto package = temp_dir() / "package";
    std::filesystem::create_directories(package);
    std::ofstream(package / "CWSDPMI.EXE") << "MZ";
    std::ofstream(package / "dirun.toml") << R"toml([payload]
machine     = false
args        = ["-dir", "{dir}"]
place       = "PROG.EXE"
beside      = ["CWSDPMI.EXE"]
files       = { "RUN.BAT" = "PROG.EXE {args} > OUT.TXT" }
status      = "file"
status-file = "RC.TXT"
output-file = "OUT.TXT"
)toml";
    auto p = dirun::profile::load(package / "dirun.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    o.prof = *p;
    o.command = {"one", "two"};
    o.emulator = child_command("fake-dos", std::vector<std::string>{});
    o.timeout = 30;
    auto r = runner::run(o);
    expect(eq(r.status, 42)) << "the status the launcher wrote, not the emulator's";
    expect(eq(r.output, std::string("ran: PROG.EXE one two > OUT.TXT with dpmi\n")))
        << "the launcher carries the program's arguments, and a delivered file is beside it";
}};

UTest long_line{"a command line longer than the console takes is written to the files disk and run from it", [] {
    fake_tools t;
    auto o = guest_options("password", t);
    const std::string word(400, 'w');
    o.command = {"say", word, "exit", "3"};
    auto r = runner::run(o);
    expect(eq(r.status, 3));
    expect(contains(r.output, word)) << "the whole line ran";
    expect(contains(r.output, "dirun-command.sh (from a script)")) << "it was not typed";
}};
