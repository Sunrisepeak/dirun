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
