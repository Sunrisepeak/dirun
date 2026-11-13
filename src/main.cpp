// dirun: run one command inside a booted disk image, or boot a payload, and return its output and
// exit status (docs/contract.md).
import std;
import dirun.cli;
import dirun.runner;

namespace {

// The run: the command's output on standard output, its status returned.
int execute(const dirun::runner::options& options) {
    try {
        auto r = dirun::runner::run(options);
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

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    return dirun::cli::run(args, execute);
}
