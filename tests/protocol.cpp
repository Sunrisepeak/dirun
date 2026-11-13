// The marker protocol (docs/contract.md "The command line").
import std;
import mcpplibs.mtest;
import dirun.profile;
import dirun.protocol;

using namespace mcpplibs::mtest;
namespace protocol = dirun::protocol;

UTest quoting{"a word is single-quoted for a POSIX shell", [] {
    expect(eq(protocol::quote_posix("a b"), std::string("'a b'")));
    expect(eq(protocol::quote_posix("it's"), std::string("'it'\\''s'")));
    expect(eq(protocol::quote_posix(""), std::string("''")));
}};

UTest wrapping{"the words of a command are one command, the environment before them", [] {
    const auto c = dirun::profile::builtin().cmd;
    auto line = protocol::wrap(c, {"echo", "hello world"}, {{"K", "v 1"}});
    expect(fatal(line.has_value()));
    expect(eq(*line, std::string("echo @@BEGIN@@; env 'K=v 1' 'echo' 'hello world' < /dev/null; echo @@STATUS=$?@@")));
    auto plain = protocol::wrap(c, {"true"}, {});
    expect(eq(*plain, std::string("echo @@BEGIN@@; 'true' < /dev/null; echo @@STATUS=$?@@")));
}};

UTest unquoted{"a profile whose shell has no quoting gives the words as they are", [] {
    auto c = dirun::profile::builtin().cmd;
    c.quote = "none";
    c.line = "{argv} & echo @@STATUS=%ERRORLEVEL%@@";
    expect(eq(*protocol::wrap(c, {"dir", "C:\\"}, {}), std::string("dir C:\\ & echo @@STATUS=%ERRORLEVEL%@@")));
}};

UTest line_ends{"CRs before an LF are one line boundary; a lone CR is one", [] {
    expect(eq(protocol::normalize("a\r\nb\r\r\nc\rd\n"), std::string("a\nb\nc\nd\n")));
    expect(eq(protocol::normalize("\r\r"), std::string("\n\n")));
}};

UTest splitting{"the output is what lies between the last begin marker and the status", [] {
    const auto c = dirun::profile::builtin().cmd;
    const std::string text =
        "echo @@BEGIN@@; 'x'; echo @@STATUS=$?@@\r\n@@BEGIN@@\r\nline 1\r\nline 2\r\n@@STATUS=3@@";
    auto r = protocol::split(text, c);
    expect(eq(r.status, 3));
    expect(eq(r.output, std::string("line 1\nline 2")));
    auto none = protocol::split("no marker here", c);
    expect(eq(none.status, -1));
}};
