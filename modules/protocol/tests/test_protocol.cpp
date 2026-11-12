// The marker protocol's semantics, one case per behaviour the frozen Python
// suite pinned (tools/reference/tests/test_protocol.py).
import std;
import dirun.protocol;
import dirun.testing;

namespace p = dirun::protocol;

int main() {
    using namespace dirun::testing;

    "escaping wraps a plain word in single quotes"_test = [] {
        expect(p::escape_sharg("hello") == "'hello'");
    };

    "escaping closes and reopens around an embedded quote"_test = [] {
        expect(p::escape_sharg("it's") == "'it'\\''s'");
    };

    "a shell-injection payload stays inside the quotes"_test = [] {
        const std::string evil = "'; rm -rf /; echo '";
        const auto wrapped = p::wrap_command({"sh", "-c", evil});
        expect(wrapped.find("'\\''; rm -rf /; echo '\\''") != std::string::npos)
            << "the payload must survive only inside single-quote escaping";
    };

    "a wrapped command names its parts in order"_test = [] {
        const auto line = p::wrap_command({"./app", "--x", "1"}, {{"FOO", "bar"}});
        expect(line.starts_with("stty -echo; echo @@BEGIN@@;"));
        expect(line.ends_with("; rc=$?; echo @@STATUS=$rc@@"));
        expect(line.find("'FOO=bar'") != std::string::npos);
        expect(line.find("'./app'") != std::string::npos);
        expect(line.find("'--x'") != std::string::npos);
        expect(line.find("'1'") != std::string::npos);
    };

    "a wrapped command without env is the whole line"_test = [] {
        expect(p::wrap_command({"true"})
               == "stty -echo; echo @@BEGIN@@; 'true'; rc=$?; echo @@STATUS=$rc@@");
    };

    "splitting keeps the payload and the rc"_test = [] {
        const auto split = p::split_output("echo @@BEGIN@@\r\nhello\r\nworld\r\n@@STATUS=7@@\r\n");
        expect(split.rc == 7);
        expect(split.output == "hello\nworld");
    };

    "splitting without a status marker says so"_test = [] {
        const auto split = p::split_output("junk @@STATUS=4");
        expect(split.rc == -1);
        expect(split.output.find("junk") != std::string::npos);
    };

    "normalising folds CRLF and a lone CR into LF"_test = [] {
        std::string data = "a\r\nb\rc\nd";
        p::normalize_output(data);
        expect(data == "a\nb\nc\nd");
    };

    "a binary payload between the markers passes through"_test = [] {
        std::string chunk = "x @@BEGIN@@";
        chunk += '\x00'; chunk += '\x01'; chunk += '\xff';
        chunk += "\r@@STATUS=255@@";
        const auto split = p::split_output(chunk);
        expect(split.rc == 255);
        expect(split.output.size() == 3);
        expect(split.output[0] == '\x00' && split.output[1] == '\x01' && split.output[2] == '\xff');
    };

    return report();
}
