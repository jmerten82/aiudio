// Minimal dependency-free test harness for aiudio.
//
// Deliberately tiny so the build stays self-contained/offline (no FetchContent).
// A richer framework (Catch2/GoogleTest) can replace this later without changing
// the test bodies much. Define tests with AIUDIO_TEST(name) and end each test
// executable with AIUDIO_TEST_MAIN().
#pragma once

#include <cstdio>
#include <vector>

namespace aiudio::test {

using TestFn = void (*)();

struct Case {
    const char* name;
    TestFn fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

inline int runAll() {
    int caseFailures = 0;
    for (const auto& c : registry()) {
        const int before = failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        if (failures() == before) {
            std::printf("[  OK  ] %s\n", c.name);
        } else {
            std::printf("[ FAIL ] %s\n", c.name);
            ++caseFailures;
        }
    }
    std::printf("\n%zu test(s), %d failed case(s), %d check failure(s)\n",
                registry().size(), caseFailures, failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace aiudio::test

#define AIUDIO_TEST(name)                                                  \
    static void name();                                                    \
    static ::aiudio::test::Registrar aiudio_registrar_##name{#name, name}; \
    static void name()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++::aiudio::test::failures();                                  \
            std::printf("    CHECK failed: %s  (%s:%d)\n", #cond,          \
                        __FILE__, __LINE__);                               \
        }                                                                  \
    } while (0)

#define REQUIRE(cond)                                                      \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++::aiudio::test::failures();                                  \
            std::printf("    REQUIRE failed: %s  (%s:%d)\n", #cond,        \
                        __FILE__, __LINE__);                               \
            return;                                                        \
        }                                                                  \
    } while (0)

#define AIUDIO_TEST_MAIN() \
    int main() { return ::aiudio::test::runAll(); }
