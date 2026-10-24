#pragma once

#include <cmath>
#include <cstdint>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace test {

inline int passed = 0;
inline int failed = 0;
inline std::string current;

template <class T>
[[nodiscard]] std::string show(const T& value) {
    if constexpr (std::is_integral_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(value);
    } else {
        return "?";
    }
}

inline void check(bool ok, std::string_view expr,
                  std::source_location loc = std::source_location::current()) {
    if (ok) {
        ++passed;
        return;
    }
    ++failed;
    std::println(stderr, "FAIL {} ({}:{}) {}", current, loc.file_name(), loc.line(), expr);
}

template <class A, class B>
inline void check_eq(const A& left, const B& right, std::string_view expr,
                     std::source_location loc = std::source_location::current()) {
    if (left == right) {
        ++passed;
        return;
    }
    ++failed;
    std::println(stderr, "FAIL {} ({}:{}) {}  left={} right={}", current, loc.file_name(), loc.line(), expr,
                 show(left), show(right));
}

inline void check_near(double left, double right, double tolerance, std::string_view expr,
                       std::source_location loc = std::source_location::current()) {
    if (std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= tolerance) {
        ++passed;
        return;
    }
    ++failed;
    std::println(stderr, "FAIL {} ({}:{}) {}  left={} right={} tol={}", current, loc.file_name(), loc.line(), expr,
                 left, right, tolerance);
}

struct Suite {
    std::string name;

    template <class F>
    void test(std::string case_name, F&& body) {
        current = name + " / " + case_name;
        try {
            body();
        } catch (const std::exception& ex) {
            ++failed;
            std::println(stderr, "FAIL {} exception: {}", current, ex.what());
        }
        current.clear();
    }
};

}  // namespace test

#define CHECK(expr) ::test::check(static_cast<bool>(expr), #expr)
#define CHECK_EQ(a, b) ::test::check_eq((a), (b), #a " == " #b)
#define CHECK_NEAR(a, b, tol) ::test::check_near((a), (b), (tol), #a " ~= " #b)
