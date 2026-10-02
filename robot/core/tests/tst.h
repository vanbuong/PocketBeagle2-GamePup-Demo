/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal dependency-free test helper: CHECK* macros count failures, TEST_MAIN returns them. */
#ifndef BALBOT_TST_H
#define BALBOT_TST_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int tst_failures;
static int tst_checks;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		tst_checks++;                                                            \
		if (!(cond)) {                                                           \
			tst_failures++;                                                  \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
		}                                                                        \
	} while (0)

#define CHECK_EQ(a, b)                                                                   \
	do {                                                                             \
		long long _a = (long long)(a), _b = (long long)(b);                      \
		tst_checks++;                                                            \
		if (_a != _b) {                                                          \
			tst_failures++;                                                  \
			printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,      \
			       __LINE__, #a, #b, _a, _b);                                \
		}                                                                        \
	} while (0)

#define CHECK_NEAR(a, b, tol)                                                            \
	do {                                                                             \
		double _a = (double)(a), _b = (double)(b), _t = (double)(tol);           \
		tst_checks++;                                                            \
		if (!(fabs(_a - _b) <= _t) || isnan(_a) || isnan(_b)) {                  \
			tst_failures++;                                                  \
			printf("  FAIL %s:%d: %s ~ %s (%g vs %g, tol %g)\n", __FILE__,   \
			       __LINE__, #a, #b, _a, _b, _t);                            \
		}                                                                        \
	} while (0)

#define RUN(fn)                                                                          \
	do {                                                                             \
		int _before = tst_failures;                                              \
		fn();                                                                    \
		printf("%s %s\n", tst_failures == _before ? "ok  " : "FAIL", #fn);       \
	} while (0)

#define TEST_MAIN_END()                                                                  \
	do {                                                                             \
		printf("%d checks, %d failures\n", tst_checks, tst_failures);            \
		return tst_failures ? 1 : 0;                                             \
	} while (0)

/* deterministic xorshift for fuzz-style tests */
static unsigned tst_rng_state = 2463534242u;
static inline unsigned tst_rand(void)
{
	tst_rng_state ^= tst_rng_state << 13;
	tst_rng_state ^= tst_rng_state >> 17;
	tst_rng_state ^= tst_rng_state << 5;
	return tst_rng_state;
}
static inline float tst_randf(void) /* uniform -1..1 */
{
	return (float)((double)tst_rand() / 2147483648.0 - 1.0);
}
#endif
