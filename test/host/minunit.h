/* minimal test harness (public domain style) */
#ifndef C6_MINUNIT_H
#define C6_MINUNIT_H

#include <stdio.h>
#include <string.h>

static int mu_tests_run = 0;
static int mu_failed = 0;

#define MU_RUN(test) do { \
    int _r = (test)(); \
    mu_tests_run++; \
    if (_r != 0) { mu_failed++; printf("FAIL %s\n", #test); } \
    else { printf("pass %s\n", #test); } \
} while (0)

#define MU_CHECK(cond) do { \
    if (!(cond)) { \
        printf("  check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        return -1; \
    } \
} while (0)

#define MU_CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("  check failed: %s == %s (%lld != %lld) at %s:%d\n", \
               #a, #b, _a, _b, __FILE__, __LINE__); \
        return -1; \
    } \
} while (0)

#define MU_REPORT(name) printf("%s: %d tests, %d failed\n", name, mu_tests_run, mu_failed)

#endif /* C6_MINUNIT_H */
