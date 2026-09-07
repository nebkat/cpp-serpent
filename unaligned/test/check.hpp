#pragma once

// Minimal assertions: one executable per test file, a failure counter, and ctest.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cstddef>
#include <cstdio>

inline int failures = 0;

inline void check(bool condition, std::string_view what) {
    if (condition) return;
    std::printf("  FAIL %.*s\n", static_cast<int>(what.size()), what.data());
    ++failures;
}

template<typename A, typename B>
inline void check_equal(const A &actual, const B &expected, std::string_view what) {
    if (actual == expected) return;
    std::printf("  FAIL %.*s\n", static_cast<int>(what.size()), what.data());
    ++failures;
}

inline void check_equal(std::string_view actual, std::string_view expected, std::string_view what) {
    if (actual == expected) return;
    std::printf("  FAIL %.*s (got \"%.*s\", want \"%.*s\")\n", static_cast<int>(what.size()), what.data(),
            static_cast<int>(actual.size()), actual.data(), static_cast<int>(expected.size()), expected.data());
    ++failures;
}

inline int report(const char *name) {
    if (failures == 0) {
        std::printf("%s: ok\n", name);
        return 0;
    }
    std::printf("%s: %d failure(s)\n", name, failures);
    return 1;
}

/** Decodes the hex literals lifted from dart-bjdata's test vectors. */
inline std::vector<std::byte> from_hex(std::string_view text) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::byte> result;
    int high = -1;
    for (const char c : text) {
        const int value = nibble(c);
        if (value < 0) continue;
        if (high < 0) {
            high = value;
        } else {
            result.push_back(static_cast<std::byte>((high << 4) | value));
            high = -1;
        }
    }
    return result;
}
