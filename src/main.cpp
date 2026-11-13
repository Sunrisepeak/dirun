// dirun: run one command inside a booted disk image, or boot a payload, and return its output and
// exit status (docs/contract.md).
import std;
import dirun.cli;
import dirun.runner;

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    auto p = dirun::cli::parse(args);
    if (p.what == dirun::cli::parsed::kind::done)
        return 0;
    if (p.what == dirun::cli::parsed::kind::error) {
        std::println(std::cerr, "dirun: {}", p.message);
        std::println(std::cerr, "dirun: see `dirun --help'");
        return 2;
    }
    try {
        auto r = dirun::runner::run(p.options);
        std::cout << r.output;
        if (!r.output.empty() && !r.output.ends_with('\n'))
            std::cout << '\n';
        std::cout.flush();
        if (r.signal)
            std::println(std::cerr, "dirun: ended by signal {}; the emulator was ended", r.signal);
        return r.status;
    } catch (const dirun::runner::error& e) {
        std::println(std::cerr, "dirun: {}", e.what());
        return e.code();
    }
}
