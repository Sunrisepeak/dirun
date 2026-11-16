// A profile: how a disk image boots and how one command runs inside it, or how a payload is booted
// and how its status comes back (docs/profiles.md). Profiles are data: a guest's prompts, its
// shell's quoting and its mount commands belong to the image, not to this program.
module;
#include <cstdint>
export module dirun.profile;

import std;
import tomlplusplus;

export namespace dirun::profile {

// THE QUOTING STYLES: how a word reaches the program that reads the line it is written into. The
// line's reader is the guest's -- a POSIX shell, the UEFI Shell, DOS's COMMAND.COM -- so the
// style is the profile's, for its command line ([command] quote) and for the arguments a payload's
// launcher file or emulator word carries ([payload] quote).
//
//   posix       each word single-quoted (safe for any byte)
//   uefi-shell  a word that is empty, or holds a blank or one of `"', `^', `%', `#', in double
//               quotes, with `^' before each of those four (UEFI Shell Specification 2.2: the
//               Shell splits a line at blanks outside quotes, and `^' makes the next character
//               literal)
//   none        the words as given; a reader with no quoting (COMMAND.COM) cannot take a blank
//               in a word
inline constexpr std::array<std::string_view, 3> quoting_styles{"posix", "uefi-shell", "none"};
inline constexpr std::string_view quoting_styles_named = "posix, uefi-shell or none";
inline bool quoting_style(std::string_view s) {
    return std::ranges::find(quoting_styles, s) != quoting_styles.end();
}

struct step {
    std::string expect;            // ECMAScript regex awaited on the console
    std::optional<std::string> send;  // written after it matched; nothing: no reply
    bool line = true;              // `send` is followed by a newline
    bool optional = false;         // passed over when the next step's pattern appears first
    double timeout = 300;          // seconds, within the run's own limit
    int pace = 0;                  // milliseconds between the characters of `send': a console
                                   // that polls its line (a boot loader's) drops faster input
};

struct machine {
    std::string arch = "x86_64";   // qemu-system-<arch>
    std::string type;              // -machine
    std::string cpu;               // -cpu
    int memory = 512;              // MiB
    int smp = 1;
    std::vector<std::string> args; // further qemu words, {var} expanded
    std::string image;             // the disk booted when --image is not given; a relative name is
                                   // the profile file's directory's (an image ships its profile)
    std::string format;            // the image's format; empty: the emulator reads it
};

struct command {
    std::vector<std::string> prepare;  // lines sent before the command, each on its own
    std::string line;                  // the command line; {env} and {argv} are filled in
    std::string env;                   // how the environment precedes the words; {assignments}
    std::string quote = "posix";       // how each word is written: a quoting style (quoting_styles)
    std::string begin;                 // the marker after which the program's output begins
    std::string status;                // a regex whose first group is the program's status
    int line_max = 0;                  // a longer line is not typed: a console's input line is
                                       // bounded (a terminal's canonical input); 0: no bound
    std::string script;                // how a line written as a file runs ({script}: its guest name)
};

struct payload {
    std::vector<std::string> args;  // the emulator's words; {payload} the file, {dir} a directory
                                    // holding it, {args} the program's arguments
    bool machine = true;            // the machine's QEMU words precede `args'; false: an emulator
                                    // that is given only `args' (one that is not QEMU)
    std::string place;              // where the file goes in {dir}; empty: no directory
    std::map<std::string, std::string> files;  // further files written into {dir}, by name, from
                                               // templates ({args}, {payload}, {dir}, {var})
    std::string status = "exit";    // exit: the emulator's status; debug-exit: (status - 1) / 2;
                                    // marker; file: read from `status_file' in {dir}
    std::string marker;             // status = marker: a regex whose first group is the status
    std::string status_file;        // status = file: the file in {dir} holding the status
    std::string output_file;        // a file in {dir} holding the program's output, read when it ends
    std::string after;              // output before this regex's first match is boot noise
    std::string quote = "none";     // how each of the program's arguments is written into {args}:
                                    // a quoting style (quoting_styles); none: as given (0.5.0)
    std::map<std::string, std::string> env;  // the emulator's environment (a display it needs none of)
    std::string emulator;           // the emulator when --emulator is not given; a relative name is
                                    // the profile file's directory's (a package ships its profile)
    std::vector<std::string> beside;  // files delivered beside the payload, from the profile's directory
    std::string stderr_file;        // a file in {dir} holding the program's standard error (0.4.0)
};

// A guest reached by commands rather than by a console this program drives (0.4.0): a device
// bridge (adb), a remote shell (ssh). `shell' runs one command line there, `push' delivers one
// file, `at' is where delivered files are placed. The guest's own command line is the profile's
// [command], as for a console.
struct reach {
    std::vector<std::string> shell;  // {line}: the command line
    std::vector<std::string> push;   // {host}: the file here; {guest}: its name there
    std::string at;
};

// How the machine a reached guest runs on starts and ends (0.4.0); absent: it is already running
// (a remote host). `command' runs for the length of the run; `ready' is run until it succeeds;
// `stop' ends the machine, else the started program is ended.
struct start {
    std::vector<std::string> command;
    std::vector<std::string> ready;
    double ready_timeout = 300;      // seconds, within the run's own limit
    std::vector<std::string> stop;
};

// The test protocol's events across the machine's boundary (docs/contract.md, "The test
// protocol"): the guest writes them to `report', and `read' and the file's name print it.
struct relay {
    std::string report;
    std::vector<std::string> read = {"cat"};
};

struct profile {
    std::string name;
    machine vm;
    std::vector<step> boot;
    command cmd;
    std::vector<std::string> mount;  // sent before the command when files are delivered
    std::string files_at;            // where the mount lines put the files disk in the guest
    std::optional<relay> events;
    std::optional<payload> boot_payload;
    std::optional<reach> reached;
    std::optional<start> lifecycle;
    std::string origin;              // the file it came from, or "built-in"
    std::string dir;                 // the directory of that file, `{profile-dir}'; empty when built in
};

// The profile the program carries, `posix-login'.
profile builtin();
// The text of the built-in profile (profiles/posix-login.toml, carried in the program).
std::string_view builtin_text();
// A profile read from TOML text; `origin' names it in errors. An unknown key is an error, for a
// profile with a misspelt key would boot and then fail somewhere else.
std::expected<profile, std::string> parse(std::string_view text, std::string_view origin);
std::expected<profile, std::string> load(const std::filesystem::path& file);
// `{name}' replaced by `vars'' value; an unknown name is an error naming it. `{{' and `}}' are
// one brace each (a shell's `${{HOME}}').
std::expected<std::string, std::string>
expand(std::string_view text, const std::map<std::string, std::string>& vars);

}  // namespace dirun::profile

namespace dirun::profile {

namespace {

constexpr std::string_view kBuiltin = R"toml(name = "posix-login"

[machine]
arch   = "x86_64"
memory = 512
smp    = 1

[[boot]]
expect  = "login: "
send    = "root"
timeout = 300

[[boot]]
expect   = "Password:"
send     = "root"
optional = true

[[boot]]
expect = "# |\\$ "

[command]
prepare = ["stty -echo"]
line    = "echo @@BEGIN@@; {env}{argv} < /dev/null; echo @@STATUS=$?@@"
env     = "env {assignments} "
quote   = "posix"
begin   = "@@BEGIN@@"
status  = "@@STATUS=(\\d+)@@"
line-max = 200
script   = "sh {script}"

[files]
mount = ["mkdir -p /mnt/dirun", "mount -r -t msdosfs /dev/vtbd1s1 /mnt/dirun 2>/dev/null || mount -r /dev/vdb1 /mnt/dirun"]
at    = "/mnt/dirun"

[relay]
report = "/tmp/dirun-mtest.ndjson"
)toml";

std::expected<void, std::string> known(const toml::table& t, std::string_view where,
                                       std::initializer_list<std::string_view> keys) {
    for (const auto& [k, v] : t)
        if (std::ranges::find(keys, k.str()) == keys.end())
            return std::unexpected(std::format("{}: `{}' is not a key of a profile ({})", where,
                                               k.str(), std::string(where)));
    return {};
}

template <class View>
std::vector<std::string> strings(const View& n) {
    std::vector<std::string> out;
    if (auto a = n.as_array())
        for (const auto& v : *a)
            if (auto s = v.template value<std::string>())
                out.push_back(*s);
    return out;
}

bool valid_regex(const std::string& r) {
    try {
        std::regex x(r, std::regex::ECMAScript);
        return true;
    } catch (const std::regex_error&) {
        return false;
    }
}

}  // namespace

std::string_view builtin_text() { return kBuiltin; }

std::expected<profile, std::string> parse(std::string_view text, std::string_view origin) {
    toml::table t;
    try {
        t = toml::parse(text, std::string(origin));
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: {}", origin, e.description()));
    }
    const std::string o(origin);
    if (auto k = known(t, o, {"name", "machine", "boot", "command", "files", "relay", "payload", "reach", "start"}); !k)
        return std::unexpected(k.error());
    profile p;
    p.origin = o;
    p.name = t["name"].value_or(std::string("unnamed"));
    if (auto m = t["machine"].as_table()) {
        if (auto k = known(*m, o + " [machine]", {"arch", "type", "cpu", "memory", "smp", "args", "image", "format"}); !k)
            return std::unexpected(k.error());
        p.vm.arch = (*m)["arch"].value_or(p.vm.arch);
        p.vm.type = (*m)["type"].value_or(std::string());
        p.vm.cpu = (*m)["cpu"].value_or(std::string());
        p.vm.memory = int((*m)["memory"].value_or(std::int64_t(p.vm.memory)));
        p.vm.smp = int((*m)["smp"].value_or(std::int64_t(p.vm.smp)));
        p.vm.args = strings((*m)["args"]);
        p.vm.image = (*m)["image"].value_or(std::string());
        p.vm.format = (*m)["format"].value_or(std::string());
    }
    if (auto b = t["boot"].as_array())
        for (const auto& n : *b) {
            auto st = n.as_table();
            if (!st)
                return std::unexpected(o + ": each [[boot]] is a table");
            if (auto k = known(*st, o + " [[boot]]", {"expect", "send", "line", "optional", "timeout", "pace"}); !k)
                return std::unexpected(k.error());
            step s;
            s.expect = (*st)["expect"].value_or(std::string());
            if (s.expect.empty() || !valid_regex(s.expect))
                return std::unexpected(std::format("{}: a [[boot]] step needs a valid `expect' pattern (got `{}')", o, s.expect));
            if (auto v = (*st)["send"].value<std::string>())
                s.send = *v;
            s.line = (*st)["line"].value_or(true);
            s.optional = (*st)["optional"].value_or(false);
            s.pace = int((*st)["pace"].value_or(std::int64_t(0)));
            s.timeout = (*st)["timeout"].value_or(300.0);
            if (auto i = (*st)["timeout"].value<std::int64_t>())
                s.timeout = double(*i);
            p.boot.push_back(std::move(s));
        }
    if (auto c = t["command"].as_table()) {
        if (auto k = known(*c, o + " [command]", {"prepare", "line", "env", "quote", "begin", "status", "line-max", "script"}); !k)
            return std::unexpected(k.error());
        p.cmd.prepare = strings((*c)["prepare"]);
        p.cmd.line = (*c)["line"].value_or(std::string());
        p.cmd.env = (*c)["env"].value_or(std::string());
        p.cmd.quote = (*c)["quote"].value_or(std::string("posix"));
        p.cmd.begin = (*c)["begin"].value_or(std::string());
        p.cmd.status = (*c)["status"].value_or(std::string());
        p.cmd.line_max = int((*c)["line-max"].value_or(std::int64_t(0)));
        p.cmd.script = (*c)["script"].value_or(std::string());
        if (p.cmd.line_max > 0 && p.cmd.script.empty())
            return std::unexpected(o + ": [command] line-max needs `script', how a line written as a file runs");
        if (!quoting_style(p.cmd.quote))
            return std::unexpected(std::format("{}: [command] quote is {}, not `{}'", o, quoting_styles_named, p.cmd.quote));
        if (p.cmd.line.empty() || p.cmd.begin.empty() || p.cmd.status.empty() || !valid_regex(p.cmd.status))
            return std::unexpected(o + ": [command] needs `line', `begin' and a valid `status' pattern");
    }
    if (auto f = t["files"].as_table()) {
        if (auto k = known(*f, o + " [files]", {"mount", "at"}); !k)
            return std::unexpected(k.error());
        p.mount = strings((*f)["mount"]);
        p.files_at = (*f)["at"].value_or(std::string());
    }
    if (auto r = t["relay"].as_table()) {
        if (auto k = known(*r, o + " [relay]", {"report", "read"}); !k)
            return std::unexpected(k.error());
        relay e;
        e.report = (*r)["report"].value_or(std::string());
        if (auto words = strings((*r)["read"]); !words.empty())
            e.read = std::move(words);
        if (e.report.empty())
            return std::unexpected(o + ": [relay] needs `report', the guest file the events are written to");
        p.events = std::move(e);
    }
    if (auto y = t["payload"].as_table()) {
        if (auto k = known(*y, o + " [payload]", {"args", "machine", "place", "files", "status", "marker",
                                                   "status-file", "output-file", "after", "env", "emulator",
                                                   "beside", "stderr-file", "quote"});
            !k)
            return std::unexpected(k.error());
        payload pl;
        pl.args = strings((*y)["args"]);
        pl.machine = (*y)["machine"].value_or(true);
        pl.place = (*y)["place"].value_or(std::string());
        if (auto f = (*y)["files"].as_table())
            for (const auto& [name, text] : *f) {
                auto v = text.value<std::string>();
                if (!v)
                    return std::unexpected(std::format("{}: [payload] files.{} is a template string", o, name.str()));
                pl.files.emplace(std::string(name.str()), *v);
            }
        if (auto e = (*y)["env"].as_table())
            for (const auto& [name, value] : *e)
                if (auto v = value.value<std::string>())
                    pl.env.emplace(std::string(name.str()), *v);
        pl.emulator = (*y)["emulator"].value_or(std::string());
        pl.beside = strings((*y)["beside"]);
        pl.status = (*y)["status"].value_or(std::string("exit"));
        pl.marker = (*y)["marker"].value_or(std::string());
        pl.status_file = (*y)["status-file"].value_or(std::string());
        pl.output_file = (*y)["output-file"].value_or(std::string());
        pl.stderr_file = (*y)["stderr-file"].value_or(std::string());
        pl.after = (*y)["after"].value_or(std::string());
        pl.quote = (*y)["quote"].value_or(std::string("none"));
        if (!quoting_style(pl.quote))
            return std::unexpected(std::format("{}: [payload] quote is {}, not `{}'", o, quoting_styles_named, pl.quote));
        if (pl.status != "exit" && pl.status != "debug-exit" && pl.status != "marker" && pl.status != "file")
            return std::unexpected(std::format("{}: [payload] status is exit, debug-exit, marker or file, not `{}'", o, pl.status));
        if (pl.status == "file" && pl.status_file.empty())
            return std::unexpected(o + ": [payload] status = \"file\" needs `status-file'");
        if (pl.status == "marker" && (pl.marker.empty() || !valid_regex(pl.marker)))
            return std::unexpected(o + ": [payload] status = \"marker\" needs a valid `marker' pattern");
        if (!pl.after.empty() && !valid_regex(pl.after))
            return std::unexpected(o + ": [payload] `after' is not a valid pattern");
        p.boot_payload = std::move(pl);
    }
    if (auto r = t["reach"].as_table()) {
        if (auto k = known(*r, o + " [reach]", {"shell", "push", "at"}); !k)
            return std::unexpected(k.error());
        reach re;
        re.shell = strings((*r)["shell"]);
        re.push = strings((*r)["push"]);
        re.at = (*r)["at"].value_or(std::string());
        if (re.shell.empty())
            return std::unexpected(o + ": [reach] needs `shell', the command that runs a line in the guest");
        if (!re.push.empty() && re.at.empty())
            return std::unexpected(o + ": [reach] `push' needs `at', where delivered files are placed");
        if (p.cmd.line.empty())
            return std::unexpected(o + ": [reach] needs [command], the guest's own command line");
        p.reached = std::move(re);
    }
    if (auto st = t["start"].as_table()) {
        if (auto k = known(*st, o + " [start]", {"command", "ready", "ready-timeout", "stop"}); !k)
            return std::unexpected(k.error());
        start sm;
        sm.command = strings((*st)["command"]);
        sm.ready = strings((*st)["ready"]);
        sm.stop = strings((*st)["stop"]);
        sm.ready_timeout = (*st)["ready-timeout"].value_or(300.0);
        if (auto i = (*st)["ready-timeout"].value<std::int64_t>())
            sm.ready_timeout = double(*i);
        if (!p.reached)
            return std::unexpected(o + ": [start] belongs to a guest reached by commands: it needs [reach]");
        if (sm.command.empty() && sm.ready.empty())
            return std::unexpected(o + ": [start] needs `command', `ready', or both");
        p.lifecycle = std::move(sm);
    }
    return p;
}

profile builtin() {
    auto p = parse(kBuiltin, "built-in");
    return std::move(*p);
}

std::expected<profile, std::string> load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::unexpected(std::format("cannot read the profile {}", file.generic_string()));
    std::stringstream text;
    text << in.rdbuf();
    auto p = parse(text.str(), file.generic_string());
    if (!p)
        return p;
    const auto here = std::filesystem::absolute(file).parent_path();
    p->dir = here.generic_string();
    auto beside_profile = [&](std::string& name) {
        if (!name.empty() && std::filesystem::path(name).is_relative())
            name = (here / name).generic_string();
    };
    beside_profile(p->vm.image);
    if (p->boot_payload) {
        beside_profile(p->boot_payload->emulator);
        for (auto& f : p->boot_payload->beside)
            beside_profile(f);
    }
    return p;
}

std::expected<std::string, std::string>
expand(std::string_view text, const std::map<std::string, std::string>& vars) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((text[i] == '{' || text[i] == '}') && i + 1 < text.size() && text[i + 1] == text[i]) {
            out += text[i++];  // `{{' and `}}' stand for one brace
            continue;
        }
        if (text[i] != '{') {
            out += text[i];
            continue;
        }
        const auto end = text.find('}', i);
        if (end == std::string_view::npos) {
            out += text.substr(i);
            break;
        }
        const std::string name(text.substr(i + 1, end - i - 1));
        auto it = vars.find(name);
        if (it == vars.end())
            return std::unexpected(std::format("`{{{}}}' has no value (give it with --var {}=...)", name, name));
        out += it->second;
        i = end;
    }
    return out;
}

}  // namespace dirun::profile
