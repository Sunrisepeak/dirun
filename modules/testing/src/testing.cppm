// The test protocol, written out rather than depended on:
//
//     import dirun.testing;
//     int main() {
//         using namespace dirun::testing;
//         "adds"_test = [] { expect(1 + 1 == 2) << "arithmetic"; };
//         return report();
//     }
//
// A case body runs now; what escapes it is caught and counted as the case's
// failure, not the program's end.
export module dirun.testing;

import std;

export namespace dirun::testing {

namespace detail {
struct summary {
    int cases = 0;
    int failed = 0;
    std::vector<std::string> messages;
};
inline summary& state() {
    static summary s;
    return s;
}
}  // namespace detail

// Skips a case on this host: counted, reported, never a failure.
inline void skip(std::string_view reason) {
    auto& s = detail::state();
    ++s.cases;
    s.messages.emplace_back("SKIP  " + std::string(reason));
}

class Expectation {
public:
    explicit Expectation(bool passed) : passed_(passed) {}
    Expectation(const Expectation&) = delete;
    Expectation& operator=(const Expectation&) = delete;
    ~Expectation() noexcept(false) {
        if (!passed_ && !std::uncaught_exceptions()) {
            ++detail::state().failed;
            if (!message_.empty())
                detail::state().messages.push_back(std::move(message_));
        }
    }

    template <class T>
    Expectation& operator<<(const T& value) {
        if (!passed_) {
            if constexpr (std::is_convertible_v<const T&, std::string_view>) {
                message_ += std::string_view{value};
            } else {
                message_ += std::format("{}", value);
            }
        }
        return *this;
    }

private:
    bool passed_;
    std::string message_;
};

inline Expectation expect(bool condition) { return Expectation{condition}; }

class TestCase {
public:
    explicit constexpr TestCase(std::string_view name) : name_(name) {}

    void operator=(const std::function<void()>& body) const {
        auto& s = detail::state();
        ++s.cases;
        const int before = s.failed;
        try {
            body();
        } catch (const std::exception& error) {
            ++s.failed;
            s.messages.push_back(std::format("threw: {}", error.what()));
        } catch (...) {
            ++s.failed;
            s.messages.push_back("threw: an unknown condition");
        }
        std::println("  {}  {}", s.failed == before ? "held" : "FAIL", name_);
    }

private:
    std::string_view name_;
};

inline constexpr TestCase operator""_test(const char* name, std::size_t size) {
    return TestCase{std::string_view{name, size}};
}

// Prints the summary and returns the process exit code.
inline int report() {
    const auto& s = detail::state();
    for (const auto& message : s.messages)
        std::println("  {}", message);
    std::println("{} case(s), {} failed", s.cases, s.failed);
    return s.failed == 0 ? 0 : 1;
}

}  // namespace dirun::testing
