// The command line's shape, one case per behaviour the frozen Python suite
// pinned (tools/reference/tests/test_cli.py): the flags, the `--` command, the
// error texts, and the exit codes for misuse (2) and version/help (0).
import std;
import dirun.cli;
import dirun.runner;
import dirun.testing;

namespace cli = dirun::cli;
namespace {

std::vector<char*> words(std::vector<std::string>& storage) {
    std::vector<char*> out;
    for (auto& word : storage) out.push_back(word.data());
    return out;
}

cli::ParseOutcome parse_words(std::vector<std::string> args) {
    args.insert(args.begin(), "dirun");
    auto mutable_args = args;
    return cli::parse(static_cast<int>(mutable_args.size()), words(mutable_args).data());
}

template <class Body>
bool errors_with(std::vector<std::string> args, const std::string& fragment, Body&& body) {
    try {
        parse_words(args);
        return false;
    } catch (const std::runtime_error& error) {
        body();
        return std::string(error.what()).find(fragment) != std::string::npos;
    }
}

}  // namespace

int main() {
    using namespace dirun::testing;

    "the image and the command, with -- stripped, are the run"_test = [] {
        const auto parsed = parse_words({"--image", "i", "--env", "A=B", "--file", "h=/g",
                                         "--", "cmd", "x"});
        expect(parsed.what == cli::ParseOutcome::kind::run);
        expect(parsed.options.image == "i");
        expect(parsed.options.command == std::vector<std::string>{"cmd", "x"});
        expect(parsed.options.env == std::vector<std::pair<std::string, std::string>>{{"A", "B"}});
        expect(parsed.options.files.size() == 1);
        expect(parsed.options.files[0].host == "h" && parsed.options.files[0].guest == "/g");
    };

    "a command without -- still arrives"_test = [] {
        const auto parsed = parse_words({"--image", "i", "cmd"});
        expect(parsed.options.command == std::vector<std::string>{"cmd"});
    };

    "help and version are their own outcomes"_test = [] {
        expect(parse_words({"--help"}).what == cli::ParseOutcome::kind::help);
        expect(parse_words({"-h"}).what == cli::ParseOutcome::kind::help);
        expect(parse_words({"--version"}).what == cli::ParseOutcome::kind::version);
    };

    "the machine, cpu, memory, smp, accel and timeout land where they belong"_test = [] {
        const auto parsed = parse_words({"--image", "i", "--machine", "virt", "--cpu", "max",
                                         "--memory", "1024", "--smp", "4", "--accel", "tcg",
                                         "--timeout", "12.5", "--", "true"});
        expect(parsed.options.machine == "virt");
        expect(parsed.options.cpu == "max");
        expect(parsed.options.memory_mib == 1024);
        expect(parsed.options.smp == 4);
        expect(parsed.options.acceleration == dirun::runner::accel::tcg);
        expect(parsed.options.timeout_seconds == 12.5);
    };

    "an unknown option is refused"_test = [] {
        const auto parsed = parse_words({"--image", "i", "--nonsense", "--", "true"});
        expect(parsed.what == cli::ParseOutcome::kind::error);
        expect(parsed.error.find("unknown option") != std::string::npos);
    };

    "a missing image is refused"_test = [] {
        const auto parsed = parse_words({"--", "true"});
        expect(parsed.what == cli::ParseOutcome::kind::error);
        expect(parsed.error.find("--image") != std::string::npos);
    };

    "a missing command is refused"_test = [] {
        const auto parsed = parse_words({"--image", "i"});
        expect(parsed.what == cli::ParseOutcome::kind::error);
        expect(parsed.error.find("command is required") != std::string::npos);
    };

    "--env without = is refused"_test = [] {
        expect(errors_with({"--image", "i", "--env", "NOEQ", "--", "true"}, "--env expects NAME=VAL", [] {}));
    };

    "--file without = is refused"_test = [] {
        expect(errors_with({"--image", "i", "--file", "noeq", "--", "true"}, "--file expects host=guest", [] {}));
    };

    "--accel outside its choices is refused"_test = [] {
        expect(errors_with({"--image", "i", "--accel", "hyper", "--", "true"}, "--accel wants", [] {}));
    };

    "the usage names every flag in its group"_test = [] {
        const std::string usage = std::string{cli::usage_head} + cli::usage_body;
        for (const auto* flag : {"--image", "--machine", "--cpu", "--memory", "--smp",
                                 "--accel", "--timeout", "--expect", "--env", "--file",
                                 "--help", "--version"})
            expect(usage.find(flag) != std::string::npos) << flag;
        expect(usage.find("run:") != std::string::npos);
        expect(usage.find("console:") != std::string::npos);
        expect(usage.find("files and environment:") != std::string::npos);
    };

    return report();
}
