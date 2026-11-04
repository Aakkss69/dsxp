/* Minimal test harness for the C core. */
#ifndef DSXP_TESTS_H
#define DSXP_TESTS_H

#include <math.h>
#include <stdio.h>
#include "core_internal.h"

extern int t_failures, t_checks;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_INT(a, b) do { long _a = (long)(a), _b = (long)(b); t_checks++; if (_a != _b) { \
    t_failures++; fprintf(stderr, "FAIL %s:%d: %s == %s (%ld vs %ld)\n", \
    __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); t_checks++; \
    if (!(fabs(_a - _b) <= (eps))) { t_failures++; fprintf(stderr, \
    "FAIL %s:%d: %s ~= %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

/* json_check.c */
bool json_valid(const char *s, size_t n);

void test_decode(void);
void test_shape(void);
void test_gesture(void);
void test_bindings(void);
void test_packets(void);
void test_output(void);
void test_json(void);
void test_api(void);
void test_core(void);

int dsxp_core_tests_run(void);

#endif
