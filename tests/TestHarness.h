#pragma once

// A deliberately tiny test harness. The parsers under test are pure functions
// over strings, so a full framework would be more dependency than value.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

inline const char*& currentCase() {
    static const char* name = "";
    return name;
}

inline void reportFailure(const char* file, int line, const std::string& message) {
    ++failureCount();
    std::fprintf(stderr, "  FAIL %s\n    %s:%d: %s\n", currentCase(), file, line,
                 message.c_str());
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back(Case{name, std::move(body)});
    }
};

inline int runAll() {
    int passed = 0;
    for (const auto& testCase : registry()) {
        currentCase() = testCase.name;
        const int before = failureCount();
        testCase.body();
        if (failureCount() == before) {
            ++passed;
        }
    }
    std::printf("\n%d/%zu test cases passed", passed, registry().size());
    if (failureCount() > 0) {
        std::printf(" (%d assertion failures)\n", failureCount());
        return 1;
    }
    std::printf("\n");
    return 0;
}

}  // namespace test

#define TEST_CASE_IMPL2(name, counter)                                   \
    static void testBody##counter();                                     \
    static const ::test::Registrar testReg##counter{name, testBody##counter}; \
    static void testBody##counter()

#define TEST_CASE_IMPL(name, counter) TEST_CASE_IMPL2(name, counter)
#define TEST_CASE(name) TEST_CASE_IMPL(name, __COUNTER__)

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) ::test::reportFailure(__FILE__, __LINE__, "CHECK(" #expr ")"); \
    } while (false)

// Like CHECK, but ends the test case: for preconditions the rest depends on.
#define REQUIRE(expr)                                                              \
    do {                                                                           \
        if (!(expr)) {                                                             \
            ::test::reportFailure(__FILE__, __LINE__, "REQUIRE(" #expr ")");       \
            return;                                                                \
        }                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                               \
    do {                                                                         \
        const auto& a_ = (actual);                                               \
        const auto& e_ = (expected);                                             \
        if (!(a_ == e_)) {                                                       \
            ::test::reportFailure(__FILE__, __LINE__,                            \
                                  std::string{#actual " == " #expected} +        \
                                      "\n      actual:   " + ::test::show(a_) +  \
                                      "\n      expected: " + ::test::show(e_));  \
        }                                                                        \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                   \
    do {                                                                          \
        const double a_ = static_cast<double>(actual);                            \
        const double e_ = static_cast<double>(expected);                          \
        if (std::abs(a_ - e_) > (tolerance)) {                                    \
            ::test::reportFailure(__FILE__, __LINE__,                             \
                                  std::string{#actual " ~= " #expected} +         \
                                      "\n      actual:   " + std::to_string(a_) + \
                                      "\n      expected: " + std::to_string(e_)); \
        }                                                                         \
    } while (false)

namespace test {

inline std::string show(const std::string& value) { return '"' + value + '"'; }
inline std::string show(std::string_view value) { return '"' + std::string{value} + '"'; }
inline std::string show(const char* value) { return std::string{"\""} + value + '"'; }
inline std::string show(bool value) { return value ? "true" : "false"; }

template <typename T>
std::string show(const T& value) {
    if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else {
        return "<value>";
    }
}

}  // namespace test
