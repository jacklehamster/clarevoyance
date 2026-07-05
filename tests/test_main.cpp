// test_main.cpp — entry point for the unit test binary (make test-unit).
#include "test_harness.h"

int g_checks = 0;
int g_failures = 0;

void test_json();
void test_serialize();
void test_events();
void test_determinism();
void test_scene_load();
void test_text();

int main() {
    test_json();
    test_serialize();
    test_events();
    test_determinism();
    test_scene_load();
    test_text();

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "OK" : "FAILED", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
