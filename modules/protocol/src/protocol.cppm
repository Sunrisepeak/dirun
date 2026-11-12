// The marker protocol, one line of POSIX sh on the wire:
//
//     stty -echo; echo @@BEGIN@@; <command...>; rc=$?; echo @@STATUS=$rc@@
//
// The bytes between the BEGIN marker and the STATUS marker are the program's
// stdout (CR/CRLF normalised to LF); nothing here knows a system.
export module dirun.protocol;

import std;

export namespace dirun::protocol {

inline constexpr std::string_view begin_marker = "@@BEGIN@@";
inline constexpr std::string_view status_marker_tail = "@@STATUS=$rc@@";

// Single-quote an argument for POSIX sh; safe for any byte content. A quote
// closes the string, becomes an escaped quote, and the string reopens: the
// exact three characters `'\\''`.
inline std::string escape_sharg(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out += '\'';
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}
inline std::string wrap_command(const std::vector<std::string>& argv,
                                const std::vector<std::pair<std::string, std::string>>& env = {}) {
    std::string line = "stty -echo; echo ";
    line += begin_marker;
    if (!env.empty()) {
        line += "; env";
        for (const auto& [name, value] : env)
            line += " " + escape_sharg(name + "=" + value);
    }
    for (const auto& arg : argv)
        line += "; " + escape_sharg(arg);
    line += "; rc=$?; echo ";
    line += status_marker_tail;
    return line;
}

// Normalise serial line endings: CRLF and a lone CR become LF.
inline void normalize_output(std::string& data) {
    std::string out;
    out.reserve(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\r') {
            if (i + 1 < data.size() && data[i + 1] == '\n') ++i;
            out += '\n';
        } else {
            out += data[i];
        }
    }
    data = std::move(out);
}

// What splitting the bytes up to a STATUS marker produced: the program stdout
// and whether a complete status marker was present (rc = -1 when it was not).
struct split_result {
    std::string output;
    int rc = -1;
};

// Split the bytes consumed up to and including a STATUS marker: everything
// before and including BEGIN is dropped (the command echo, the prelude), the
// rest is the program's stdout, normalised.
inline split_result split_output(const std::string& chunk) {
    static const std::regex status_re{R"(@@STATUS=(\d+)@@)"};
    split_result result;
    std::smatch match;
    if (!std::regex_search(chunk, match, status_re)) {
        result.output = chunk;
        normalize_output(result.output);
        return result;
    }
    std::string head = chunk.substr(0, match.position());
    const auto begin_at = head.find(begin_marker);
    if (begin_at != std::string::npos)
        head.erase(0, begin_at + begin_marker.size());
    // One line boundary on each side of the payload, no more.
    while (!head.empty() && (head.front() == '\r' || head.front() == '\n'))
        head.erase(head.begin());
    while (!head.empty() && (head.back() == '\r' || head.back() == '\n'))
        head.pop_back();
    result.rc = std::stoi(match[1].str());
    result.output = std::move(head);
    normalize_output(result.output);
    return result;
}

}  // namespace dirun::protocol
