#include "tests.h"

int t_failures, t_checks;

int dsxp_core_tests_run(void) {
    t_failures = t_checks = 0;
    test_decode();
    test_shape();
    test_gesture();
    test_motion_cal();
    test_motion();
    test_motion_yaw();
    test_motion_calib();
    test_bindings();
    test_config();
    test_config_store();
    test_packets();
    test_output();
    test_json();
    test_api();
    test_core();
    fprintf(stderr, "dsxp core tests: %d checks, %d failures\n", t_checks, t_failures);
    return t_failures;
}
