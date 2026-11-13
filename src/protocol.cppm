// The marker protocol: one command line written to the guest's shell, its output read between a
// begin marker and a status marker. What the line looks like -- the markers, the quoting, how the
// environment precedes the words -- is the profile's (docs/contract.md); nothing here knows a
// system.
export module dirun.protocol;

import std;
import dirun.profile;

export namespace dirun::protocol {

// A word for a POSIX shell, single-quoted: safe for any byte. A quote closes the string, becomes
// an escaped quote, and the string reopens: the three characters '\''.
std::string quote_posix(std::string_view word) {
    std::string out = "'";
    for (char c : word)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// The command line the profile describes, for `argv' with `env' (NAME, value) before it. The words
// are joined by spaces: they are one command.
std::expected<std::string, std::string>
wrap(const profile::command& c, const std::vector<std::string>& argv,
     const std::vector<std::pair<std::string, std::string>>& env) {
    auto word = [&](std::string_view w) {
        return c.quote == "posix" ? quote_posix(w) : std::string(w);
    };
    std::string words;
    for (const auto& a : argv)
        words += (words.empty() ? "" : " ") + word(a);
    std::string assignments;
    for (const auto& [k, v] : env)
        assignments += (assignments.empty() ? "" : " ") + word(k + "=" + v);
    std::string env_part;
    if (!env.empty()) {
        auto e = profile::expand(c.env, {{"assignments", assignments}});
        if (!e)
            return std::unexpected(e.error());
        if (c.env.empty())
            return std::unexpected("the profile's [command] states no `env', and --env was given");
        env_part = *e;
    }
    return profile::expand(c.line, {{"env", env_part}, {"argv", words}});
}

// Serial line endings normalised: CRs before an LF and the LF are one line boundary, and a lone CR
// becomes LF. A console that adds a CR to a line the guest ended with CR LF (a terminal's `onlcr',
// a text-mode stream on Windows) writes CR CR LF, which is one boundary, not two.
std::string normalize(std::string_view data) {
    std::string out;
    out.reserve(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\r') {
            std::size_t j = i;
            while (j < data.size() && data[j] == '\r')
                ++j;
            if (j < data.size() && data[j] == '\n')
                i = j;
            out += '\n';
        } else {
            out += data[i];
        }
    }
    return out;
}

struct split_result {
    std::string output;  // the program's output, normalised
    int status = -1;     // -1: no status marker in the text
};

// The text read up to and including the status marker, split: what precedes the last begin
// marker before the status (the echo of the line, a prompt) is dropped, and one line boundary on
// each side of the output.
split_result split(std::string_view text, const profile::command& c) {
    split_result r;
    const std::regex status_re(c.status, std::regex::ECMAScript);
    std::match_results<std::string_view::const_iterator> m;
    if (!std::regex_search(text.begin(), text.end(), m, status_re)) {
        r.output = normalize(text);
        return r;
    }
    auto head = text.substr(0, std::size_t(m.position(0)));
    if (const auto at = head.rfind(c.begin); at != std::string_view::npos)
        head.remove_prefix(at + c.begin.size());
    while (!head.empty() && (head.front() == '\r' || head.front() == '\n'))
        head.remove_prefix(1);
    while (!head.empty() && (head.back() == '\r' || head.back() == '\n'))
        head.remove_suffix(1);
    r.output = normalize(head);
    r.status = m.size() > 1 && m[1].matched ? std::stoi(m[1].str()) : 0;
    return r;
}

}  // namespace dirun::protocol
