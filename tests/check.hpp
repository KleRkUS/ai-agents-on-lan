#pragma once

// Minimal test harness: TEST registers a case, CHECK records failures without aborting.

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace aio::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace aio::test

#define AIO_CONCAT_(a, b) a##b
#define AIO_CONCAT(a, b) AIO_CONCAT_(a, b)

#define TEST(name)                                                                      \
    static void name();                                                                 \
    static ::aio::test::Registrar AIO_CONCAT(registrar_, name)(#name, name);            \
    static void name()

#define CHECK(expr)                                                                     \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            ++::aio::test::failures();                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #expr "\n"; \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        const auto va_ = (a);                                                           \
        const auto vb_ = (b);                                                           \
        if (!(va_ == vb_)) {                                                            \
            ++::aio::test::failures();                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " == " #b \
                      << "\n  left:  " << va_ << "\n  right: " << vb_ << "\n";          \
        }                                                                               \
    } while (0)
