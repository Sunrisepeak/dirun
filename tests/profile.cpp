// Profiles are data (docs/profiles.md).
import std;
import mcpplibs.mtest;
import dirun.profile;

using namespace mcpplibs::mtest;
namespace profile = dirun::profile;

namespace {
std::filesystem::path repository() {
    return std::filesystem::path(std::source_location::current().file_name()).parent_path().parent_path();
}
}  // namespace

UTest builtin{"the built-in profile is the shipped posix-login file", [] {
    auto shipped = profile::load(repository() / "profiles" / "posix-login.toml");
    expect(fatal(shipped.has_value())) << (shipped ? "" : shipped.error());
    const auto b = profile::builtin();
    expect(eq(b.name, shipped->name));
    expect(fatal(eq(b.boot.size(), shipped->boot.size())));
    for (std::size_t i = 0; i < b.boot.size(); ++i) {
        expect(eq(b.boot[i].expect, shipped->boot[i].expect));
        expect(eq(b.boot[i].send.value_or("<none>"), shipped->boot[i].send.value_or("<none>")));
        expect(eq(b.boot[i].optional, shipped->boot[i].optional));
    }
    expect(eq(b.cmd.line, shipped->cmd.line));
    expect(eq(b.cmd.status, shipped->cmd.status));
    expect(eq(b.mount, shipped->mount));
}};

UTest unknown{"a key a profile does not have is an error", [] {
    auto p = profile::parse("[machine]\narhc = \"x86_64\"\n", "t.toml");
    expect(fatal(!p.has_value()));
    expect(contains(p.error(), "arhc"));
    expect(!profile::parse("[[boot]]\nexpect = \"(\"\n", "t.toml").has_value()) << "an invalid pattern";
    expect(!profile::parse("[payload]\nstatus = \"guess\"\n", "t.toml").has_value());
    expect(!profile::parse("[payload]\nstatus = \"marker\"\n", "t.toml").has_value()) << "marker needs a pattern";
}};

UTest payload{"a payload profile states its arguments and how its status returns", [] {
    auto p = profile::parse(R"([payload]
args = ["-bios", "{firmware}", "-drive", "format=raw,file=fat:rw:{dir}"]
place = "EFI/BOOT/BOOTX64.EFI"
status = "debug-exit"
)", "uefi.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    expect(fatal(p->boot_payload.has_value()));
    expect(eq(p->boot_payload->status, std::string("debug-exit")));
    expect(eq(p->boot_payload->place, std::string("EFI/BOOT/BOOTX64.EFI")));
}};

UTest variables{"a variable is expanded, a brace doubled is one brace, a missing one is named", [] {
    expect(eq(*profile::expand("-bios {fw}", {{"fw", "/x.fd"}}), std::string("-bios /x.fd")));
    expect(eq(*profile::expand("echo ${{HOME}}", {}), std::string("echo ${HOME}")));
    auto missing = profile::expand("{nope}", {});
    expect(fatal(!missing.has_value()));
    expect(contains(missing.error(), "--var nope="));
}};

UTest shipped_with_image{"an image's profile names its disk beside it, where files go, and how events come back", [] {
    const auto dir = temp_dir();
    std::ofstream(dir / "image.toml") << R"(name = "shipped"
[machine]
image = "disk.qcow2"
[files]
mount = ["mount it"]
at    = "/mnt/in"
[relay]
report = "/tmp/events.ndjson"
read   = ["cat", "--"]
)";
    auto p = profile::load(dir / "image.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    expect(eq(std::filesystem::path(p->vm.image), std::filesystem::absolute(dir) / "disk.qcow2"))
        << "a relative image is the profile's directory's";
    expect(eq(p->files_at, std::string("/mnt/in")));
    expect(fatal(p->events.has_value()));
    expect(eq(p->events->report, std::string("/tmp/events.ndjson")));
    expect(eq(p->events->read, std::vector<std::string>{"cat", "--"}));
    expect(contains(profile::parse("[relay]\nread = [\"cat\"]\n", "t").error(), "report"));
}};

UTest payload_package{"a payload profile names its emulator and the files beside it from its own directory", [] {
    const auto dir = temp_dir() / "pkg";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "dirun.toml") << R"([payload]
machine  = false
emulator = "bin/dosbox"
beside   = ["CWSDPMI.EXE"]
env      = { SDL_VIDEODRIVER = "dummy" }
)";
    auto p = profile::load(dir / "dirun.toml");
    expect(fatal(p.has_value() && p->boot_payload.has_value())) << (p ? "" : p.error());
    expect(eq(std::filesystem::path(p->boot_payload->emulator), std::filesystem::absolute(dir) / "bin/dosbox"));
    expect(eq(std::filesystem::path(p->boot_payload->beside.front()), std::filesystem::absolute(dir) / "CWSDPMI.EXE"));
    expect(eq(p->boot_payload->env.at("SDL_VIDEODRIVER"), std::string("dummy")));
}};

UTest examples{"each example profile of the documentation is a profile", [] {
    const auto dir = std::filesystem::path(std::source_location::current().file_name()).parent_path().parent_path() /
                     "docs" / "examples";
    int seen = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".toml")
            continue;
        ++seen;
        auto p = dirun::profile::load(e.path());
        expect(p.has_value()) << e.path().generic_string() << ": " << (p ? "" : p.error());
        if (p)
            expect(p->reached.has_value()) << e.path().generic_string() << " states how its guest is reached";
    }
    expect(eq(seen, 2));
}};

UTest disk{"a payload's disk is its directory or a FAT image, and an image is not read back", [] {
    auto p = profile::parse("[payload]\nargs = [\"-drive\", \"file={disk}\"]\ndisk = \"fat-image\"\nstatus = \"debug-exit\"\n", "t.toml");
    expect(fatal(p.has_value())) << (p ? "" : p.error());
    expect(eq(p->boot_payload->disk, std::string("fat-image")));
    expect(eq(profile::parse("[payload]\n", "t.toml")->boot_payload->disk, std::string("directory")));
    expect(!profile::parse("[payload]\ndisk = \"vhd\"\n", "t.toml").has_value());
    auto read_back = profile::parse("[payload]\ndisk = \"fat-image\"\nstatus = \"file\"\nstatus-file = \"RC.TXT\"\n", "t.toml");
    expect(fatal(!read_back.has_value()));
    expect(contains(read_back.error(), "not read back"));
    expect(!profile::parse("[payload]\ndisk = \"fat-image\"\noutput-file = \"OUT.TXT\"\n", "t.toml").has_value());
}};

UTest disk_bus{"the bus of the disks is the profile's: virtio unless it states ide", [] {
    auto unstated = profile::parse("[machine]\narch = \"x86_64\"\n", "t.toml");
    expect(fatal(unstated.has_value())) << (unstated ? "" : unstated.error());
    expect(eq(unstated->vm.bus, std::string("virtio")));
    auto ide = profile::parse("[machine]\nbus = \"ide\"\n", "t.toml");
    expect(fatal(ide.has_value())) << (ide ? "" : ide.error());
    expect(eq(ide->vm.bus, std::string("ide")));
    auto other = profile::parse("[machine]\nbus = \"floppy\"\n", "t.toml");
    expect(fatal(!other.has_value()));
    expect(contains(other.error(), "virtio or ide"));
}};

UTest files_form{"the files disk is a FAT volume unless the profile states a tar archive", [] {
    auto unstated = profile::parse("[files]\nmount = [\"true\"]\nat = \"/in\"\n", "t.toml");
    expect(fatal(unstated.has_value())) << (unstated ? "" : unstated.error());
    expect(eq(unstated->files_form, std::string("fat")));
    auto tar = profile::parse("[files]\nform = \"tar\"\nmount = [\"tar xf /dev/wd1\"]\nat = \"/in\"\n", "t.toml");
    expect(fatal(tar.has_value())) << (tar ? "" : tar.error());
    expect(eq(tar->files_form, std::string("tar")));
    auto other = profile::parse("[files]\nform = \"zip\"\n", "t.toml");
    expect(fatal(!other.has_value()));
    expect(contains(other.error(), "fat or tar"));
}};
