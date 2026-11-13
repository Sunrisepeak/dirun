// The command line, the same on every system (docs/contract.md):
//
//     dirun [--image IMG] [--profile FILE] [--var K=V]... [machine] [--env K=V]... [--pass NAME|P*]...
//           [--file H=G]... -- COMMAND...
//     dirun --payload F [--profile FILE] [payload] [--var K=V]... [machine] [--file H=G]... [-- ARGS...]
export module dirun.cli;

import std;
import mcpplibs.cmdline;
import mcpplibs.os;
import dirun.profile;
import dirun.runner;

export namespace dirun::cli {

inline constexpr std::string_view version = "0.3.0";

struct parsed {
    enum class kind { run, done, error } what = kind::error;
    runner::options options;
    std::string message;  // the error; empty when help or version was printed
};

parsed parse(std::span<const std::string> args);

}  // namespace dirun::cli

namespace dirun::cli {

namespace cmdline = mcpplibs::cmdline;
namespace os = mcpplibs::os;

namespace {

cmdline::App app() {
    cmdline::App a("dirun");
    (void)a.version(std::string(version))
        .description("Run one command inside a booted disk image, or boot a payload, and return its "
                     "output and exit code.");
    a.option("image").takes_value().value_name("IMG").help("the disk image (never written: a temporary overlay is booted)")
        .option("payload").takes_value().value_name("FILE").help("boot FILE itself (a firmware application, a bare-metal program); needs a profile with [payload]")
        .option("profile").takes_value().value_name("FILE").help("how the image boots and runs a command, or how the payload boots (default: the built-in posix-login)")
        .option("var").takes_value().multiple().value_name("NAME=VALUE").help("a value for {NAME} in the profile")
        .option("arch").takes_value().value_name("ARCH").help("qemu-system-ARCH (default: the profile's)")
        .option("emulator").takes_value().value_name("PROGRAM").help("the emulator to run (default: qemu-system-ARCH found on PATH)")
        .option("machine").takes_value().value_name("M").help("qemu machine type")
        .option("cpu").takes_value().value_name("C").help("qemu cpu model")
        .option("memory").takes_value().value_name("MiB").help("guest memory")
        .option("smp").takes_value().value_name("N").help("guest processors")
        .option("accel").takes_value().value_name("auto|kvm|tcg").help("acceleration (default auto: the host's, else tcg)")
        .option("timeout").takes_value().value_name("SEC").help("the run's time limit (default 300)")
        .option("env").takes_value().multiple().value_name("NAME=VALUE").help("set for the command (repeatable)")
        .option("pass").takes_value().multiple().value_name("NAME|PREFIX*").help("give the command this machine's variable NAME, or every one beginning PREFIX (repeatable)")
        .option("emulator-arg").takes_value().multiple().value_name("WORD").help("payload: a word of the emulator's command line ({payload}, {dir} and {var} expanded); repeatable")
        .option("place").takes_value().value_name("PATH").help("payload: where the file goes in {dir}")
        .option("status").takes_value().value_name("exit|debug-exit|marker").help("payload: how its status returns")
        .option("marker").takes_value().value_name("REGEX").help("payload: status = marker, a pattern whose first group is the status")
        .option("after").takes_value().value_name("REGEX").help("payload: output before this pattern is boot noise")
        .option("trace").takes_value().value_name("FILE").help("write the console's transcript to FILE")
        .option("file").takes_value().multiple().value_name("HOST=GUEST").help("deliver a host file: on a read-only FAT disk to a guest, beside a payload (repeatable)")
        .end();
    return a;
}

std::optional<std::pair<std::string, std::string>> pair_of(std::string_view word) {
    const auto eq = word.find('=');
    if (eq == std::string_view::npos || eq == 0)
        return std::nullopt;
    return std::pair{std::string(word.substr(0, eq)), std::string(word.substr(eq + 1))};
}

std::optional<double> number(const std::string& text) {
    double v = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || p != text.data() + text.size() || v <= 0)
        return std::nullopt;
    return v;
}

}  // namespace

parsed parse(std::span<const std::string> args) {
    parsed out;
    auto a = app().parse_from(args);
    if (!a) {
        if (a.error().is_error())
            out.message = a.error().message;
        else
            out.what = parsed::kind::done;
        return out;
    }
    auto& o = out.options;
    auto fail = [&](std::string m) {
        out.what = parsed::kind::error;
        out.message = std::move(m);
        return out;
    };
    o.image = a->value("image").value_or("");
    o.payload = a->value("payload").value_or("");
    if (!o.image.empty() && !o.payload.empty())
        return fail("name one of --image (run a command in a guest) or --payload (boot a payload)");
    if (auto p = a->value("profile")) {
        auto loaded = profile::load(*p);
        if (!loaded)
            return fail(loaded.error());
        o.prof = std::move(*loaded);
    }
    // A PAYLOAD'S DESCRIPTION, AS OPTIONS. A board states how its payload boots and how the status
    // returns in its runner statement, where a file of its own has no name to be reached by; the
    // options say what a profile's [payload] says, and add to it.
    auto payload_options = [&]() -> std::optional<std::string> {
        const bool any = a->opts.contains("emulator-arg") || a->value("place") || a->value("status") ||
                         a->value("marker") || a->value("after");
        if (!any)
            return std::nullopt;
        if (o.payload.empty())
            return std::string("--emulator-arg, --place, --status, --marker and --after describe a payload");
        if (!o.prof.boot_payload)
            o.prof.boot_payload = profile::payload{};
        auto& pl = *o.prof.boot_payload;
        if (auto it = a->opts.find("emulator-arg"); it != a->opts.end())
            pl.args.insert(pl.args.end(), it->second.values.begin(), it->second.values.end());
        if (auto v = a->value("place")) pl.place = *v;
        if (auto v = a->value("status")) pl.status = *v;
        if (auto v = a->value("marker")) pl.marker = *v;
        if (auto v = a->value("after")) pl.after = *v;
        if (pl.status != "exit" && pl.status != "debug-exit" && pl.status != "marker")
            return std::format("--status is exit, debug-exit or marker, not `{}'", pl.status);
        if (pl.status == "marker" && pl.marker.empty())
            return std::string("--status marker needs --marker");
        return std::nullopt;
    };
    if (auto e = payload_options())
        return fail(*e);
    if (!o.payload.empty() && !o.prof.boot_payload)
        return fail("--payload needs its description: a profile with [payload] (--profile) or the "
                    "payload options (--emulator-arg, --status, ...), which are the board's");
    auto pairs = [&](std::string_view name, auto&& each) -> std::optional<std::string> {
        if (auto it = a->opts.find(name); it != a->opts.end())
            for (const auto& w : it->second.values) {
                auto kv = pair_of(w);
                if (!kv)
                    return std::format("--{} takes NAME=VALUE, not `{}'", name, w);
                each(*kv);
            }
        return std::nullopt;
    };
    if (auto e = pairs("var", [&](auto kv) { o.vars[kv.first] = kv.second; }))
        return fail(*e);
    // This machine's variables the command is given (--pass NAME, --pass PREFIX*).
    if (auto it = a->opts.find("pass"); it != a->opts.end())
        for (const auto& entry : os::env::variables()) {
            const auto eq = entry.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            const auto name = entry.substr(0, eq);
            for (const auto& pattern : it->second.values)
                if (pattern.ends_with('*') ? name.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1))
                                           : name == pattern) {
                    o.env.emplace_back(name, entry.substr(eq + 1));
                    break;
                }
        }
    // The test protocol of whatever started this program: a test runner's events come back.
    if (auto v = os::env::get("MTEST_PROTOCOL"), r = os::env::get("MTEST_REPORT");
        v && !v->empty() && r && !r->empty())
        o.protocol = runner::options::test_protocol{*v, *r, os::env::get("MTEST_MODE").value_or(""),
                                                    os::env::get("MTEST_SELECT").value_or("")};
    if (auto e = pairs("env", [&](auto kv) { o.env.push_back(kv); }))
        return fail(*e);
    if (auto e = pairs("file", [&](auto kv) { o.files.push_back({kv.first, kv.second}); }))
        return fail(*e);
    o.trace = a->value("trace").value_or("");
    o.arch = a->value("arch");
    // A runner that installs its emulator names it: a package's program is not on PATH.
    if (auto v = a->value("emulator"); v && !v->empty())
        o.emulator = {*v};
    o.machine = a->value("machine");
    o.cpu = a->value("cpu");
    for (auto [name, field] : {std::pair{"memory", &o.memory}, std::pair{"smp", &o.smp}})
        if (auto v = a->value(name)) {
            auto n = number(*v);
            if (!n)
                return fail(std::format("--{} takes a positive number, not `{}'", name, *v));
            *field = int(*n);
        }
    if (auto v = a->value("timeout")) {
        auto n = number(*v);
        if (!n)
            return fail(std::format("--timeout takes a positive number of seconds, not `{}'", *v));
        o.timeout = *n;
    }
    if (auto v = a->value("accel")) {
        if (*v == "auto") o.acceleration = runner::accel::auto_;
        else if (*v == "kvm") o.acceleration = runner::accel::kvm;
        else if (*v == "tcg") o.acceleration = runner::accel::tcg;
        else return fail(std::format("--accel is auto, kvm or tcg, not `{}'", *v));
    }
    o.command = a->positionals;
    if (o.payload.empty() && o.image.empty() && o.prof.vm.image.empty())
        return fail("name an image with --image (or a profile whose [machine] states one), or a payload with --payload");
    if (o.payload.empty() && o.command.empty())
        return fail("name the command to run after `--'");
    out.what = parsed::kind::run;
    return out;
}

}  // namespace dirun::cli
