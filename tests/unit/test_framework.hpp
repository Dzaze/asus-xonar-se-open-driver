// Mini-cadre de test sans dépendance externe : enregistrement automatique des cas,
// assertions avec localisation, résumé en fin d'exécution. Suffisant pour dsp-core ;
// remplaçable par Catch2/GoogleTest plus tard sans réécrire les tests (macros compatibles).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace xonar::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failureCount() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
    ++failureCount();
    std::printf("    ECHEC %s:%d : %s\n", file, line, message.c_str());
}

inline int runAll() {
    int failedCases = 0;
    for (const auto& c : registry()) {
        const int before = failureCount();
        std::printf("[ %s ]\n", c.name);
        c.fn();
        if (failureCount() != before) ++failedCases;
    }
    std::printf("\n%zu cas, %d en échec, %d assertion(s) en échec.\n", registry().size(), failedCases, failureCount());
    return failedCases == 0 ? 0 : 1;
}

} // namespace xonar::test

#define XONAR_CONCAT_INNER(a, b) a##b
#define XONAR_CONCAT(a, b) XONAR_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                      \
    static void XONAR_CONCAT(xonar_test_fn_, __LINE__)();                                     \
    static ::xonar::test::Registrar XONAR_CONCAT(xonar_test_reg_, __LINE__)(                  \
        name, &XONAR_CONCAT(xonar_test_fn_, __LINE__));                                       \
    static void XONAR_CONCAT(xonar_test_fn_, __LINE__)()

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) ::xonar::test::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")");  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        const double xa_ = static_cast<double>(a), xb_ = static_cast<double>(b);             \
        if (std::fabs(xa_ - xb_) > static_cast<double>(eps))                                 \
            ::xonar::test::reportFailure(__FILE__, __LINE__,                                 \
                std::string("CHECK_NEAR(" #a ", " #b ") : ") + std::to_string(xa_) +         \
                " vs " + std::to_string(xb_));                                               \
    } while (0)

#define REQUIRE(cond)                                                                        \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            ::xonar::test::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");         \
            return;                                                                          \
        }                                                                                    \
    } while (0)
