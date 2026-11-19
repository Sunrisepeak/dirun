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
    bool snapshot = false;
    for (const auto& a : args)
        snapshot = snapshot || a.find("snapshot=on") != std::string::npos;
    for (const auto& a : args)
        if (auto at = a.find("file=fat:"); at != std::string::npos) {
            const auto dir = a.substr(at + 9, a.find(',', at) - at - 9);
            for (const auto& e : std::filesystem::directory_iterator(dir))
                disk += " " + e.path().filename().string();
        }
    // The files disk as an image of its own (0.7.0): a tar archive from its first sector, whose
    // names are read here by the format's rule, or the FAT32 image an IDE guest is given; and the
    // interface the disks are on.
    for (const auto& a : args) {
        const auto at = a.find("file=");
        if (at == std::string::npos || a.find("file=fat:") != std::string::npos)
            continue;
        const auto file = a.substr(at + 5, a.find(',', at) - at - 5);
        if (file.ends_with("files.tar")) {
            // The archive from its first ustar header; what precedes it is a label (0.8.0), whose
            // first bytes, and the header's offset, are reported, with the disk's size.
            std::ifstream in(file, std::ios::binary);
            std::array<char, 512> h{};
            std::uint64_t at = 0;
            bool header = false;
            while (in.read(h.data(), 512)) {
                if (std::string_view(h.data() + 257, 5) == "ustar") {
                    header = true;
                    break;
                }
                if (at == 0 && h[0] != 0)
                    disk += " label:" + std::string(h.data(), std::find(h.data(), h.data() + 16, char{0}));
                at += 512;
            }
            if (header && at != 0)
                disk += " archive-at:" + std::to_string(at);
            while (header && h[0] != 0) {
                const std::string name(h.data(), std::find(h.data(), h.data() + 100, char{0}));
                const auto size = std::stoull(std::string(h.data() + 124, 11), nullptr, 8);
                disk += " tar:" + name + (std::string_view(h.data() + 257, 5) == "ustar" ? "" : "(not ustar)");
                in.seekg(static_cast<std::streamoff>((size + 511) / 512 * 512), std::ios::cur);
                if (!in.read(h.data(), 512))
                    break;
            }
            if (!header)
                disk += " tar:(no ustar header)";
            disk += " disk-bytes:" + std::to_string(std::filesystem::file_size(file));
        } else if (file.ends_with("files.img")) {
            std::ifstream in(file, std::ios::binary);
            std::vector<char> head(1 << 20 | 512);
            in.read(head.data(), std::streamsize(head.size()));
            disk += in && std::string_view(head.data() + (1 << 20) + 82, 8) == "FAT32   " ? " fat-image" : " no-image";
        }
        if (a.find("if=ide") != std::string::npos && file.ends_with("base.qcow2"))
            disk += " image-on-ide";
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
        if (snapshot)
            out += " snapshot";
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

// A firmware given a disk (0.6.0): it finds `file=' naming the image, checks that the image is a
// disk with a FAT32 partition, and reports the payload's status as a debug-exit device would.
child firmware_disk{"fake-firmware-disk", [](std::vector<std::string> args) {
    std::string image;
    for (const auto& a : args)
        if (auto at = a.find("file="); at != std::string::npos)
            image = a.substr(at + 5);
    std::ifstream in(image, std::ios::binary);
    std::vector<char> head(1 << 20 | 512);
    in.read(head.data(), std::streamsize(head.size()));
    const bool fat32 = in && std::string_view(head.data() + (1 << 20) + 82, 8) == "FAT32   " &&
                       static_cast<unsigned char>(head[510]) == 0x55;
    std::cout << "BOOTED\r\n" << (fat32 ? "a FAT32 disk" : "no disk") << "\r\n" << std::flush;
    return 2 * 6 + 1;
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
    const bool data = std::filesystem::exists(dir / "DATA" / "D.TXT");
    std::ofstream(dir / "OUT.TXT") << "ran: " << launcher << (dpmi ? " with dpmi" : "") << (data ? " with data" : "")
                                   << "\r\n";
    std::ofstream(dir / "ERR.TXT") << "a warning\r\n";
    std::ofstream(dir / "RC.TXT") << "42\r\n";
    return 0;
}};

// A guest reached by commands (0.4.0), played by one program: `push HOST GUEST' copies a file into
// the guest's file system (a directory here), `shell ROOT LINE' runs a [command] line there,
// `machine ROOT' is the machine (ready a second after it starts), `ready ROOT' succeeds once it is,
// and `stop ROOT' records that it was asked to stop.
child reached{"fake-reach", [](std::vector<std::string> args) {
    if (args.empty())
        return 9;
    const std::string what = args[0];
    if (what == "push" && args.size() == 3) {
        std::filesystem::create_directories(std::filesystem::path(args[2]).parent_path());
        std::filesystem::copy_file(args[1], args[2], std::filesystem::copy_options::overwrite_existing);
        return 0;
    }
    if (what == "machine" && args.size() == 2) {
        std::this_thread::sleep_for(std::chrono::seconds{1});
        std::ofstream(std::filesystem::path(args[1]) / "booted") << "1";
        std::this_thread::sleep_for(std::chrono::seconds{120});
        return 0;
    }
    if (what == "ready" && args.size() == 2)
        return std::filesystem::exists(std::filesystem::path(args[1]) / "booted") ? 0 : 1;
    if (what == "stop" && args.size() == 2) {
        std::ofstream(std::filesystem::path(args[1]) / "stopped") << "1";
        return 0;
    }
    if (what != "shell" || args.size() != 3)
        return 9;
    const std::filesystem::path root = args[1];
    const std::string line = args[2];
    if (!line.starts_with("echo @@BEGIN@@;")) {  // a prepared line
        std::ofstream(root / "prepared", std::ios::app) << line << "\n";
        return 0;
    }
    auto w = words_of(line);
    std::string out;
    int status = 0;
    if (!w.empty() && w.front().ends_with("/prog")) {  // a delivered program: the guest's files
        for (const auto& e : std::filesystem::recursive_directory_iterator(root / "at"))
            if (e.is_regular_file())
                out += std::filesystem::relative(e.path(), root / "at").generic_string() + " ";
    } else
        for (std::size_t i = 0; i < w.size(); ++i) {
            if (w[i] == "exit" && i + 1 < w.size())
                status = std::stoi(w[++i]);
            else if (w[i] != "say")
                out += w[i] + " ";
        }
    std::cout << "@@BEGIN@@\n" << out << "\n@@STATUS=" << status << "@@\n";
    std::cerr << "from the guest's error stream\n";
    return 0;
}};

namespace {

runner::options reached_options(const std::string& extra) {
    runner::options o;
    const auto root = temp_dir() / "guest";
    std::filesystem::create_directories(root);
    auto self = child_command("fake-reach", std::vector<std::string>{});
    auto quoted = [&](std::vector<std::string> words) {
        std::string t = "[";
        for (const auto& w : words)
            t += (t.size() > 1 ? ", " : "") + std::string("'") + w + "'";
        return t + "]";
    };
    auto with = [&](std::vector<std::string> tail) {
        auto words = self;
        words.insert(words.end(), tail.begin(), tail.end());
        return quoted(words);
    };
    const std::string text = std::format(R"toml([command]
prepare = ["mkdir -p {{root}}/at"]
line    = "echo @@BEGIN@@; {{env}}{{argv}} < /dev/null; echo @@STATUS=$?@@"
env     = "env {{assignments}} "
begin   = "@@BEGIN@@"
status  = '@@STATUS=(\d+)@@'

[reach]
shell = {}
push  = {}
at    = "/at"
{})toml", with({"shell", "{root}", "{line}"}), with({"push", "{host}", "{root}{guest}"}), extra);
    auto p = dirun::profile::parse(text, "reach.toml");
    if (!p)
        throw std::runtime_error(p.error());
    o.prof = *p;
    o.vars = {{"root", root.generic_string()}};
    o.timeout = 60;
    return o;
}

runner::options guest_options(std::string mode) {
    runner::options o;
    o.image = (temp_dir() / "base.qcow2").generic_string();
    std::ofstream(o.image) << "base";
    o.emulator = child_command("fake-guest", std::vector<std::string>{std::move(mode)});
    o.timeout = 30;
    return o;
}

}  // namespace

UTest status{"the command's output and status pass through", [] {
    auto o = guest_options("password");
    o.command = {"say", "hello", "exit", "7"};
    o.env = {{"V", "env-value"}};
    auto r = runner::run(o);
    expect(eq(r.status, 7));
    expect(contains(r.output, "hello"));
    expect(contains(r.output, "env-value")) << "the environment reaches the command";
}};

UTest snapshot{"the image is booted in snapshot mode: the emulator keeps the writes and the image is not written", [] {
    auto o = guest_options("password");
    o.command = {"say", "x"};
    auto r = runner::run(o);
    expect(contains(r.output, "snapshot")) << "the drive is attached with snapshot=on";
}};

UTest optional_step{"an optional step is passed over when the next prompt comes first", [] {
    auto o = guest_options("nopassword");
    o.command = {"say", "ok", "exit", "0"};
    auto r = runner::run(o);
    expect(eq(r.status, 0));
    expect(contains(r.output, "ok"));
}};

UTest codes{"each way a run cannot finish has its code", [] {
    auto timeout = guest_options("silent");
    timeout.command = {"x"};
    timeout.timeout = 2;
    try {
        (void)runner::run(timeout);
        fail("no timeout");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_timeout)) << e.what();
    }
    auto dies = guest_options("dies");
    dies.command = {"x"};
    try {
        (void)runner::run(dies);
        fail("no console end");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_no_console)) << e.what();
    }
    auto missing = guest_options("password");
    missing.command = {"x"};
    missing.image = (temp_dir() / "absent.qcow2").generic_string();
    try {
        (void)runner::run(missing);
        fail("a missing image ran");
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_unusable));
    }
    auto noqemu = guest_options("password");
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

UTest payload_disk{"a payload profile with disk = \"fat-image\" gives the emulator a FAT32 disk as {disk}", [] {
    runner::options o;
    o.payload = (temp_dir() / "app.efi").generic_string();
    std::ofstream(o.payload) << "MZ";
    auto p = dirun::profile::parse(R"([payload]
args = ["-drive", "if=virtio,format=raw,file={disk}"]
place = "APP.EFI"
files = { "startup.nsh" = "APP.EFI {args}" }
disk = "fat-image"
status = "debug-exit"
after = "BOOTED\n"
)", "uefi.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    o.prof = *p;
    o.emulator = child_command("fake-firmware-disk", std::vector<std::string>{});
    o.timeout = 30;
    auto r = runner::run(o);
    expect(eq(r.status, 6));
    expect(eq(r.output, std::string("a FAT32 disk\n")));
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
    auto o = guest_options("password");
    const auto program = temp_dir() / "prog";
    std::ofstream(program) << "binary";
    o.command = {program.generic_string(), "arg"};
    auto r = runner::run(o);
    expect(contains(r.output, "/mnt/dirun/prog arg")) << "the guest runs the copy, by the profile's [files] at";
    expect(contains(r.output, "disk: prog")) << "the program is on the read-only FAT disk the emulator was given";
}};

UTest delivered_tar{"with [files] form = \"tar\" the files disk is an archive of the delivered files", [] {
    auto o = guest_options("password");
    o.prof.files_form = "tar";
    const auto program = temp_dir() / "prog";
    std::ofstream(program) << "binary";
    o.command = {program.generic_string(), "arg"};
    auto r = runner::run(o);
    expect(contains(r.output, "tar:prog")) << r.output;
    expect(!contains(r.output, "not ustar")) << r.output;
}};

UTest delivered_tar_label{"a tar files disk behind the system's own label: the label first, the archive at the "
                          "offset, the disk of the stated size", [] {
    auto o = guest_options("password");
    o.prof.files_form = "tar";
    const auto label = temp_dir() / "files-label.bin";
    std::ofstream(label, std::ios::binary) << "SYSTEMLABEL";
    o.prof.files_label = label.generic_string();
    o.prof.files_offset = 4096;
    o.prof.files_size = 1u << 20;
    const auto program = temp_dir() / "prog";
    std::ofstream(program) << "binary";
    o.command = {program.generic_string(), "arg"};
    auto r = runner::run(o);
    expect(contains(r.output, "label:SYSTEMLABEL")) << r.output;
    expect(contains(r.output, "archive-at:4096")) << r.output;
    expect(contains(r.output, "tar:prog")) << r.output;
    expect(contains(r.output, "disk-bytes:1048576")) << "the disk is the label's size, not the archive's: " << r.output;
    expect(eq(r.status, 0));
}};

UTest delivered_tar_too_large{"files that do not fit the files disk the label states are refused before the machine "
                              "starts", [] {
    auto o = guest_options("password");
    o.prof.files_form = "tar";
    o.prof.files_offset = 4096;
    o.prof.files_size = 8192;
    const auto program = temp_dir() / "prog";
    std::ofstream(program) << std::string(8192, 'x');
    o.command = {program.generic_string()};
    try {
        auto r = runner::run(o);
        expect(false) << "the run started: " << r.output;
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_unusable)) << e.what();
        expect(contains(std::string(e.what()), "which holds 4096")) << e.what();
    }
    o.prof.files_size = 0;
    o.prof.files_label = (temp_dir() / "no-such-label.bin").generic_string();
    try {
        auto r = runner::run(o);
        expect(false) << "the run started without its label: " << r.output;
    } catch (const runner::error& e) {
        expect(eq(e.code(), runner::exit_unusable)) << e.what();
        expect(contains(std::string(e.what()), "label could not be read")) << e.what();
    }
}};

UTest delivered_ide{"with [machine] bus = \"ide\" both disks are on it, and the files are a FAT32 image", [] {
    auto o = guest_options("password");
    o.prof.vm.bus = "ide";
    const auto program = temp_dir() / "prog";
    std::ofstream(program) << "binary";
    o.command = {program.generic_string(), "arg"};
    auto r = runner::run(o);
    expect(contains(r.output, "fat-image")) << r.output;
    expect(contains(r.output, "image-on-ide")) << r.output;
}};

UTest relayed{"a test runner's events written in the guest are appended to its report", [] {
    auto o = guest_options("password");
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

UTest payload_quoting{"a payload's arguments reach its launcher file in the profile's quoting style", [] {
    runner::options o;
    o.payload = (temp_dir() / "hello.exe").generic_string();
    std::ofstream(o.payload) << "MZ";
    auto p = dirun::profile::parse(R"toml([payload]
machine     = false
args        = ["-dir", "{dir}"]
place       = "PROG.EXE"
files       = { "RUN.BAT" = "PROG.EXE {args} > OUT.TXT" }
status      = "file"
status-file = "RC.TXT"
output-file = "OUT.TXT"
quote       = "uefi-shell"
)toml", "shell.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    o.prof = *p;
    o.command = {"one", "two three"};
    o.emulator = child_command("fake-dos", std::vector<std::string>{});
    o.timeout = 30;
    auto r = runner::run(o);
    expect(contains(r.output, "ran: PROG.EXE one \"two three\" > OUT.TXT")) << r.output;
    expect(!dirun::profile::parse("[payload]\nquote = \"cmd\"\n", "bad.toml").has_value())
        << "a quoting style the profile cannot name is refused";
}};

UTest long_line{"a command line longer than the console takes is written to the files disk and run from it", [] {
    auto o = guest_options("password");
    const std::string word(400, 'w');
    o.command = {"say", word, "exit", "3"};
    auto r = runner::run(o);
    expect(eq(r.status, 3));
    expect(contains(r.output, word)) << "the whole line ran";
    expect(contains(r.output, "dirun-command.sh (from a script)")) << "it was not typed";
}};

UTest reached_guest{"a guest reached by commands runs the command line and its status comes back", [] {
    auto o = reached_options("");
    o.command = {"say", "hello", "exit", "6"};
    auto r = runner::run(o);
    expect(eq(r.status, 6));
    expect(contains(r.output, "hello"));
    expect(contains(r.error_output, "from the guest's error stream")) << "the shell's standard error is kept apart";
    std::ifstream prepared(temp_dir() / "guest" / "prepared");
    std::string first;
    std::getline(prepared, first);
    expect(contains(first, "mkdir -p")) << "the prepared lines ran through the shell first";
}};

UTest reached_files{"a program and its runtime files are pushed into a reached guest, beside each other", [] {
    auto o = reached_options("");
    const auto program = temp_dir() / "prog";
    std::ofstream(program) << "binary";
    const auto data = temp_dir() / "d.txt";
    std::ofstream(data) << "data";
    o.command = {program.generic_string()};
    o.runtime_files = {{data.generic_string(), "data/d.txt"}, {data.generic_string(), "../above.txt"}};
    auto r = runner::run(o);
    expect(eq(r.status, 0));
    expect(contains(r.output, "prog")) << r.output;
    expect(contains(r.output, "data/d.txt")) << "beside the program, at its destination";
    expect(!contains(r.output, "above.txt")) << "a destination above the program's directory has no place";
}};

UTest reached_lifecycle{"a reached guest's machine is started, awaited until ready, and stopped", [] {
    auto self = child_command("fake-reach", std::vector<std::string>{});
    auto quoted = [&](std::vector<std::string> tail) {
        auto words = self;
        words.insert(words.end(), tail.begin(), tail.end());
        std::string t = "[";
        for (const auto& w : words)
            t += (t.size() > 1 ? ", " : "") + std::string("'") + w + "'";
        return t + "]";
    };
    auto o = reached_options(std::format("\n[start]\ncommand = {}\nready = {}\nstop = {}\nready-timeout = 30\n",
                                         quoted({"machine", "{root}"}), quoted({"ready", "{root}"}),
                                         quoted({"stop", "{root}"})));
    o.command = {"say", "up"};
    auto r = runner::run(o);
    expect(eq(r.status, 0));
    expect(std::filesystem::exists(temp_dir() / "guest" / "booted")) << "the command ran once the machine was ready";
    expect(std::filesystem::exists(temp_dir() / "guest" / "stopped")) << "the machine was asked to stop";
}};

UTest payload_files{"a payload's runtime files go beside it, and its standard error is read from its file", [] {
    runner::options o;
    o.payload = (temp_dir() / "hello.exe").generic_string();
    std::ofstream(o.payload) << "MZ";
    const auto data = temp_dir() / "d.txt";
    std::ofstream(data) << "data";
    o.runtime_files = {{data.generic_string(), "DATA/D.TXT"}};
    auto p = dirun::profile::parse(R"toml([payload]
machine     = false
args        = ["-dir", "{dir}"]
place       = "PROG.EXE"
files       = { "RUN.BAT" = "PROG.EXE {args} > OUT.TXT" }
status      = "file"
status-file = "RC.TXT"
output-file = "OUT.TXT"
stderr-file = "ERR.TXT"
)toml", "dos.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    o.prof = *p;
    o.emulator = child_command("fake-dos", std::vector<std::string>{});
    o.timeout = 30;
    auto r = runner::run(o);
    expect(contains(r.output, "with data")) << "the runtime file is beside the program";
    expect(eq(r.error_output, std::string("a warning\n"))) << "the program's standard error, apart from its output";
}};
