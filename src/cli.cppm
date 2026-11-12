// The command line, one shape on every system:
//
//     dirun --image <img> [--machine m] [--cpu c] [--memory MiB] [--smp n]
//           [--accel auto|kvm|tcg] [--timeout SEC] [--expect FILE.json]
//           [--env NAME=VAL]... [--file host=guest]... -- <command...>
export module dirun.cli;

import std;
import dirun.runner;

export namespace dirun::cli {

inline constexpr std::string_view version = "0.2.0";

struct Options {
    std::string image;
    std::vector<std::string> command;
    std::optional<std::string> machine;
    std::optional<std::string> cpu;
    int memory_mib = 512;
    int smp = 1;
    runner::accel acceleration = runner::accel::auto_;
    double timeout_seconds = 300.0;
    std::optional<std::string> expect_script;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<runner::FileDelivery> files;
};

inline const char* const usage_head =
    "usage: dirun [-h] [--version] --image IMG [--machine M] [--cpu C]\n"
    "             [--memory MiB] [--smp N] [--accel {auto,kvm,tcg}]\n"
    "             [--timeout SEC] [--expect FILE.json] [--env NAME=VAL]\n"
    "             [--file host=guest] ... -- command...\n"
    "\n"
    "Boot a disk image and run a command inside it over the serial console.\n";

inline const char* const usage_body =
    "\nrun:\n"
    "  --image IMG         base disk image (never written; a temp qcow2 overlay is used)\n"
    "  --machine M         qemu machine type (default per architecture)\n"
    "  --cpu C             qemu cpu model\n"
    "  --memory MiB        guest memory in MiB (default 512)\n"
    "  --smp N             guest vcpu count (default 1)\n"
    "  --accel ACCEL       acceleration: auto, kvm or tcg (default auto)\n"
    "  --timeout SEC       overall wall-clock budget (default 300)\n"
    "\nconsole:\n"
    "  --expect FILE.json  replace the default login script with a JSON array of\n"
    "                      {expect, send, timeout}\n"
    "\nfiles and environment:\n"
    "  --env NAME=VAL      pass NAME=VAL to the guest command (repeatable,\n"
    "                      explicit allowlist only)\n"
    "  --file host=guest   deliver host file into a read-only FAT second disk at\n"
    "                      guest path (repeatable)\n"
    "\noptions:\n"
    "  -h, --help          show this message and exit\n"
    "  --version           show the version and exit\n";

struct ParseOutcome {
    enum class kind { run, help, version, error };
    kind what = kind::error;
    Options options;
    std::string error;
};

inline ParseOutcome parse(int argc, char** argv) {
    Options parsed;
    bool have_command_separator = false;
    for (int at = 1; at < argc; ++at) {
        std::string_view word{argv[at]};
        if (have_command_separator || word == "--") {
            have_command_separator = true;
            if (word != "--") parsed.command.emplace_back(word);
            continue;
        }
        const auto value = [&](void) -> std::string {
            if (at + 1 >= argc) throw std::runtime_error{std::string{"option "} + std::string{word} + " wants a value"};
            return argv[++at];
        };
        if (word == "-h" || word == "--help") return {.what = ParseOutcome::kind::help};
        if (word == "--version") return {.what = ParseOutcome::kind::version};
        if (word == "--image") { parsed.image = value(); continue; }
        if (word == "--machine") { parsed.machine = value(); continue; }
        if (word == "--cpu") { parsed.cpu = value(); continue; }
        if (word == "--memory") { parsed.memory_mib = std::stoi(value()); continue; }
        if (word == "--smp") { parsed.smp = std::stoi(value()); continue; }
        if (word == "--accel") {
            auto name = value();
            if (name == "auto") parsed.acceleration = runner::accel::auto_;
            else if (name == "kvm") parsed.acceleration = runner::accel::kvm;
            else if (name == "tcg") parsed.acceleration = runner::accel::tcg;
            else throw std::runtime_error{"--accel wants auto, kvm or tcg"};
            continue;
        }
        if (word == "--timeout") { parsed.timeout_seconds = std::stod(value()); continue; }
        if (word == "--expect") { parsed.expect_script = value(); continue; }
        if (word == "--env") {
            auto item = value();
            const auto equals = item.find('=');
            if (equals == std::string::npos)
                throw std::runtime_error{"--env expects NAME=VAL"};
            parsed.env.emplace_back(item.substr(0, equals), item.substr(equals + 1));
            continue;
        }
        if (word == "--file") {
            auto item = value();
            const auto equals = item.find('=');
            if (equals == std::string::npos)
                throw std::runtime_error{"--file expects host=guest"};
            parsed.files.push_back({item.substr(0, equals), item.substr(equals + 1)});
            continue;
        }
        if (word.front() == '-') {
            return {.what = ParseOutcome::kind::error,
                    .error = "unknown option: " + std::string{word}};
        }
        // A bare word (no -- before it) begins the command, as the reference's
        // command line did: the rest, whatever it is, is the command.
        parsed.command.emplace_back(word);
        for (++at; at < argc; ++at) parsed.command.emplace_back(argv[at]);
        break;
    }
    if (parsed.image.empty())
        return {.what = ParseOutcome::kind::error,
                .error = "the --image option is required"};
    if (parsed.command.empty())
        return {.what = ParseOutcome::kind::error,
                .error = "a command is required (use: dirun ... -- command...)"};
    return {.what = ParseOutcome::kind::run, .options = std::move(parsed)};
}

}  // namespace dirun::cli
