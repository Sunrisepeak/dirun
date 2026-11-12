// dirun: boot a disk image, drive its console, return the program's output and
// exit code. The exit-code space is the contract's (see dirun.runner).
#include <cstdio>
#include <exception>

import std;
import dirun.cli;
import dirun.console;
import dirun.runner;

int main(int argc, char** argv) {
    const auto parsed = dirun::cli::parse(argc, argv);
    using kind = dirun::cli::ParseOutcome::kind;
    try {
        switch (parsed.what) {
            case kind::help:
                std::fputs(dirun::cli::usage_head, stdout);
                std::fputs(dirun::cli::usage_body, stdout);
                return 0;
            case kind::version:
                std::println("dirun {}", dirun::cli::version);
                return 0;
            case kind::error:
                std::println(stderr, "dirun: {}", parsed.error);
                std::println(stderr, "try 'dirun --help'");
                return 2;
            case kind::run: break;
        }

        dirun::runner::RunOptions options;
        options.image = parsed.options.image;
        options.command = parsed.options.command;
        options.machine = parsed.options.machine;
        options.cpu = parsed.options.cpu;
        options.memory_mib = parsed.options.memory_mib;
        options.smp = parsed.options.smp;
        options.acceleration = parsed.options.acceleration;
        options.timeout_seconds = parsed.options.timeout_seconds;
        options.expect_script = parsed.options.expect_script;
        options.env = parsed.options.env;
        options.files = parsed.options.files;

        const auto result = dirun::runner::run(options);
        std::fwrite(result.output.data(), 1, result.output.size(), stdout);
        std::fflush(stdout);
        return result.exit_code;
    } catch (const dirun::runner::RunnerError& error) {
        std::println(stderr, "dirun: {}", error.message());
        return error.exit_code();
    } catch (const dirun::console::ConsoleInterrupted& interrupt) {
        std::println(stderr, "dirun: ended by signal {}", interrupt.signum());
        return dirun::runner::exit_for_signal(interrupt.signum());
    } catch (const std::exception& error) {
        std::println(stderr, "dirun: {}", error.what());
        return 2;
    }
}
