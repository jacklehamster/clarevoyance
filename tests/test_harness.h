// test_harness.h — tiny zero-dependency assert harness for the unit tests.
//
// One binary, plain asserts, no GL/SDL. A failed CHECK prints its location and
// expression, marks the run failed, and keeps going so one run reports every
// failure. main() (test_main.cpp) exits 1 if anything failed.
#pragma once

#include <cstdio>

extern int g_checks;    // total CHECKs executed
extern int g_failures;  // CHECKs that failed

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define CHECK_MSG(cond, msg)                                               \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s — %s\n", __FILE__, __LINE__,       \
                        #cond, msg);                                       \
        }                                                                  \
    } while (0)
