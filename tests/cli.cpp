// The command line (docs/contract.md).
import std;
import mcpplibs.mtest;
import dirun.cli;

using namespace mcpplibs::mtest;
using run_kind = dirun::cli::parsed::kind;

namespace {
dirun::cli::parsed parse(std::vector<std::string> words) {
    words.insert(words.begin(), "dirun");
    return dirun::cli::parse(words);
}
}  // namespace

UTest guest{"a guest run takes an image, repeated settings and the command after --", [] {
    auto p = parse({"--image", "os.qcow2", "--env", "A=1", "--env", "B=two words", "--var", "x=y",
                    "--file", "h.txt=/g.txt", "--timeout", "60", "--accel", "tcg", "--", "echo", "--flag"});
    expect(fatal(eq(p.what, run_kind::run))) << p.message;
    expect(eq(p.options.image, std::string("os.qcow2")));
    expect(eq(p.options.env.size(), std::size_t(2)));
    expect(eq(p.options.env[1].second, std::string("two words")));
    expect(eq(p.options.vars.at("x"), std::string("y")));
    expect(eq(p.options.files.front().guest, std::string("/g.txt")));
    expect(eq(p.options.timeout, 60.0));
    expect(eq(p.options.command, std::vector<std::string>{"echo", "--flag"}));
}};

UTest refusals{"a command line that cannot run says why", [] {
    expect(contains(parse({"--", "true"}).message, "--image"));
    expect(contains(parse({"--image", "a", "--payload", "b", "--", "true"}).message, "one of"));
    expect(contains(parse({"--payload", "a.efi"}).message, "description"));
    expect(contains(parse({"--image", "a", "--status", "exit", "--", "x"}).message, "describe a payload"));
    expect(contains(parse({"--payload", "a.efi", "--status", "guess"}).message, "exit, debug-exit or marker"));
    expect(contains(parse({"--image", "a"}).message, "command"));
    expect(contains(parse({"--image", "a", "--env", "noequals", "--", "x"}).message, "NAME=VALUE"));
    expect(contains(parse({"--image", "a", "--timeout", "-3", "--", "x"}).message, "positive"));
    expect(contains(parse({"--image", "a", "--accel", "fast", "--", "x"}).message, "auto, kvm or tcg"));
}};

UTest payload_options{"a payload is described by options as a profile describes it", [] {
    auto p = parse({"--payload", "app.efi", "--emulator-arg", "-bios", "--emulator-arg", "/fw.fd", "--place",
                    "EFI/BOOT/BOOTX64.EFI", "--status", "debug-exit"});
    expect(fatal(eq(p.what, run_kind::run))) << p.message;
    expect(fatal(p.options.prof.boot_payload.has_value()));
    expect(eq(p.options.prof.boot_payload->args, std::vector<std::string>{"-bios", "/fw.fd"}));
    expect(eq(p.options.prof.boot_payload->status, std::string("debug-exit")));
}};

UTest emulator_named{"the emulator is the one named, else qemu-system-ARCH", [] {
    auto named = parse({"--emulator", "/pkg/bin/qemu-system-x86_64", "--image", "a", "--", "x"});
    expect(fatal(eq(named.what, run_kind::run))) << named.message;
    expect(eq(named.options.emulator, std::vector<std::string>{"/pkg/bin/qemu-system-x86_64"}));
    auto found = parse({"--image", "a", "--", "x"});
    expect(found.options.emulator.empty());
}};

UTest passed_variables{"--pass gives the command this machine's variables, by name or prefix", [] {
    auto p = parse({"--image", "a", "--pass", "PATH", "--pass", "NO_SUCH_VARIABLE_*", "--", "x"});
    expect(fatal(eq(p.what, run_kind::run))) << p.message;
    expect(fatal(eq(p.options.env.size(), std::size_t(1))));
    expect(eq(p.options.env.front().first, std::string("PATH")));
}};

UTest image_of_profile{"a profile that states its image needs no --image", [] {
    std::ofstream(temp_dir() / "image-profile.toml") << R"toml([machine]
image = "disk.qcow2"
[[boot]]
expect = "# "
[command]
line   = "{argv}"
begin  = "B"
status = "S(\\d+)"
)toml";
    auto p = parse({"--profile", (temp_dir() / "image-profile.toml").generic_string(), "--", "true"});
    expect(fatal(eq(p.what, run_kind::run))) << p.message;
    expect(p.options.image.empty()) << "the profile's image is the runner's to take";
    expect(contains(p.options.prof.vm.image, "disk.qcow2"));
}};
