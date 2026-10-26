// Minimal assertions for host tests: keep going on failure, exit code = failures.
#pragma once

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            test_failures++;                                              \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        long long a_ = (long long)(a), b_ = (long long)(b);                                \
        if (a_ != b_) {                                                                    \
            printf("%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_);  \
            test_failures++;                                                               \
        }                                                                                  \
    } while (0)

#define CHECK_BYTES(a, b, n)                                                 \
    do {                                                                     \
        if (memcmp((a), (b), (n)) != 0) {                                    \
            printf("%s:%d: bytes %s != %s\n", __FILE__, __LINE__, #a, #b);   \
            test_failures++;                                                 \
        }                                                                    \
    } while (0)

static inline int test_summary(const char *name)
{
    printf("%s: %s (%d failure(s))\n", name, test_failures ? "FAIL" : "OK", test_failures);
    return test_failures ? 1 : 0;
}
