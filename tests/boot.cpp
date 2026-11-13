// A real boot (docs/contract.md): an image named by DIRUN_TEST_IMAGE, with the profile
// DIRUN_TEST_PROFILE (default: the built-in), under the host's qemu. Skipped where either is
// absent; CI's boot job provides both.
import std;
import mcpplibs.mtest;
import mcpplibs.mtest.e2e;
import mcpplibs.os;

using namespace mcpplibs::mtest;

E2E boot{"a command runs inside a booted image", timeout{std::chrono::minutes{10}}, [] {
    const auto image = mcpplibs::os::env::get("DIRUN_TEST_IMAGE").value_or("");
    if (image.empty())
        skip("DIRUN_TEST_IMAGE names no image");
    if (!mcpplibs::os::env::find_executable("qemu-system-x86_64"))
        skip("no qemu-system-x86_64 on PATH");
    std::vector<std::string> argv{e2e::program("dirun"), "--image", image, "--timeout", "540"};
    if (auto p = mcpplibs::os::env::get("DIRUN_TEST_PROFILE"); p && !p->empty())
        argv.insert(argv.end(), {"--profile", *p});
    argv.insert(argv.end(), {"--env", "WORD=two words", "--", "sh", "-c", "echo \"[$WORD]\"; exit 9"});
    auto r = e2e::run({.argv = argv, .timeout = std::chrono::minutes{10}});
    expect(e2e::exited(r, 9));
    expect(e2e::contains(r.out, "[two words]"));
}};
