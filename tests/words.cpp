// The contract carries no vocabulary of a particular ecosystem: dirun is usable by any CI. The build
// configuration may name what it builds with, a source its imports and namespace aliases, and the
// README the commands that build it; nothing else may.
import std;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

namespace {
std::filesystem::path repository() {
    return std::filesystem::path(std::source_location::current().file_name()).parent_path().parent_path();
}
}  // namespace

UTest words{"no ecosystem's words outside the build configuration", [] {
    const std::regex banned{R"(mcpp|mcpplibs|openkal|xim(?![a-z])|xlings)", std::regex::icase};
    const std::regex source_allowance{R"(import\s+mcpplibs|mcpplibs::|import\s+nlohmann|import\s+tomlplusplus)"};
    const std::regex prose_allowance{R"(\bmcpp (build|run|test)\b|the mcpp build)"};
    const std::set<std::string> scanned{".md", ".cppm", ".cpp", ".toml"};
    std::vector<std::string> found;
    for (const auto& e : std::filesystem::recursive_directory_iterator(repository())) {
        const auto& p = e.path();
        const auto rel = p.lexically_relative(repository()).generic_string();
        if (!e.is_regular_file() || !scanned.contains(p.extension().string()) || rel.starts_with("target/") ||
            rel.starts_with(".") || p.filename() == "mcpp.toml" || rel == "tests/words.cpp")
            continue;
        std::ifstream in(p);
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            if (!std::regex_search(line, banned))
                continue;
            if (std::regex_search(line, source_allowance) || std::regex_search(line, prose_allowance))
                continue;
            found.push_back(std::format("{}:{}: {}", rel, n, line));
        }
    }
    expect(eq(found.size(), std::size_t(0))) << [&] {
        std::string s;
        for (const auto& f : found) s += f + "\n";
        return s;
    }();
}};
