/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/throughput.c / src/libltntstools/throughput.h.
 * Builds against ../src/throughput.c only.
 *
 * ltntstools_throughput_write()/write_value() and _expire_per_second_stats()
 * key all state transitions off real wall-clock time_t (via time(&now)),
 * comparing it against the struct's own stream->Bps_last_update field. Since
 * the struct is public and its fields are all plain data, these tests drive
 * state transitions deterministically -- without sleep()ing across real
 * second boundaries -- by seeding Bps_last_update into the past (to force a
 * rollover/expiry) or leaving it at "now" (to suppress one), then letting
 * the code under test read the real clock as it normally would.
 *
 * A REAL BUG was found in ltntstools_throughput_reset() while writing these
 * tests and has been FIXED in src/throughput.c as part of this change:
 * reset() zeroed byteCount/mbps/mBps but left Bps, Bps_window and
 * Bps_last_update untouched. Since ltntstools_throughput_get_bps() and
 * ltntstools_throughput_get_value() read Bps (not mbps), a caller that
 * reset() a stream and immediately queried get_bps()/get_value() would
 * still observe the pre-reset throughput for up to ~2 more seconds
 * (until _expire_per_second_stats' own staleness check kicked in),
 * even though get_mbps() correctly reported 0 the whole time. Confirmed via
 * repro: write() enough bytes to roll Bps to a nonzero value, call reset(),
 * then immediately call get_bps() -- returned the stale pre-reset value
 * before the fix, 0 after. Fixed by having reset() also zero Bps,
 * Bps_window and Bps_last_update, matching a freshly-zeroed struct.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "libltntstools/throughput.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

static int approx_eq(double a, double b, double eps)
{
	double d = a - b;
	if (d < 0)
		d = -d;
	return d <= eps;
}

/* -------- ltntstools_throughput_write_value -------- */

static void test_write_value_first_call_only_fills_window(void)
{
	/* A freshly-zeroed struct has Bps_last_update == 0, so the very first
	 * call always sees "now != 0" and takes the rollover branch -- but
	 * since Bps_window was already 0, that rollover is a no-op for Bps,
	 * and the new value only lands in Bps_window until the next
	 * rollover. */
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));

	time_t before = time(NULL);
	ltntstools_throughput_write_value(&s, 100);
	time_t after = time(NULL);

	CHECK(s.Bps == 0);
	CHECK(s.Bps_window == 100);
	CHECK(s.mbps == 0);
	CHECK(s.Bps_last_update >= before && s.Bps_last_update <= after);
}

static void test_write_value_rollover_moves_window_to_bps(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps_window = 5000000;
	s.Bps_last_update = time(NULL) - 10; /* force a rollover */

	time_t before = time(NULL);
	ltntstools_throughput_write_value(&s, 42);
	time_t after = time(NULL);

	CHECK(s.Bps == 5000000);
	CHECK(s.Bps_window == 42);
	/* write_value()'s mbps is Bps/1e6 -- no *8 bit conversion, unlike
	 * write(). This is existing, intentional behavior for this entry
	 * point (the caller-supplied "value" isn't defined to be bytes). */
	CHECK(approx_eq(s.mbps, 5.0, 0.0001));
	CHECK(s.Bps_last_update >= before && s.Bps_last_update <= after);
}

static void test_write_value_accumulates_within_same_second(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps_last_update = time(NULL); /* "now", so no rollover this call */

	ltntstools_throughput_write_value(&s, 10);
	ltntstools_throughput_write_value(&s, 20);
	ltntstools_throughput_write_value(&s, 30);

	CHECK(s.Bps_window == 60);
	CHECK(s.Bps == 0);
}

/* -------- ltntstools_throughput_write -------- */

static void test_write_rollover_computes_mbps_with_8x(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps_window = 1000000; /* 1,000,000 bytes in the prior window */
	s.Bps_last_update = time(NULL) - 10;

	uint8_t buf[10];
	ltntstools_throughput_write(&s, buf, sizeof(buf));

	CHECK(s.Bps == 1000000);
	CHECK(s.Bps_window == 10);
	/* write()'s mbps is (bytes * 8) / 1e6 -- true megabits/sec. */
	CHECK(approx_eq(s.mbps, 8.0, 0.0001));
}

static void test_write_accumulates_within_same_second(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps_last_update = time(NULL);

	uint8_t buf[100];
	ltntstools_throughput_write(&s, buf, 100);
	ltntstools_throughput_write(&s, buf, 50);

	CHECK(s.Bps_window == 150);
	CHECK(s.Bps == 0);
}

/* -------- getters / per-second expiry -------- */

static void test_get_value_returns_bps_when_fresh(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps = 12345;
	s.Bps_last_update = time(NULL); /* fresh: not > +2s old */

	CHECK(ltntstools_throughput_get_value(&s) == 12345);
}

static void test_get_bps_multiplies_value_by_8(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps = 1000;
	s.Bps_last_update = time(NULL);

	CHECK(ltntstools_throughput_get_bps(&s) == 8000);
}

static void test_get_mbps_returns_stored_field_when_fresh(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.mbps = 3.5;
	s.Bps_last_update = time(NULL);

	CHECK(approx_eq(ltntstools_throughput_get_mbps(&s), 3.5, 0.0001));
}

static void test_expire_clears_stats_after_stale_period(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps = 999;
	s.Bps_window = 50;
	s.mbps = 7.0;
	s.mBps = 2.0;
	s.Bps_last_update = time(NULL) - 5; /* well past the 2s expiry */

	CHECK(ltntstools_throughput_get_value(&s) == 0);
	CHECK(s.Bps == 0);
	CHECK(s.Bps_window == 0);
	CHECK(s.mbps == 0);
	CHECK(s.mBps == 0);
}

static void test_expire_leaves_stats_when_within_window(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps = 999;
	s.Bps_last_update = time(NULL) - 1; /* recent, must not expire */

	CHECK(ltntstools_throughput_get_value(&s) == 999);
}

static void test_get_bps_reflects_expiry_too(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps = 500;
	s.Bps_last_update = time(NULL) - 5;

	CHECK(ltntstools_throughput_get_bps(&s) == 0);
}

/* -------- ltntstools_throughput_reset -------- */

static void test_reset_zeroes_byte_count_and_mbps(void)
{
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.byteCount = 999;
	s.mbps = 4.2;
	s.mBps = 1.1;

	ltntstools_throughput_reset(&s);

	CHECK(s.byteCount == 0);
	CHECK(s.mbps == 0);
	CHECK(s.mBps == 0);
}

static void test_reset_clears_bps_so_get_value_and_get_bps_report_zero(void)
{
	/* Regression test for the fixed bug: a fresh (unexpired) nonzero Bps
	 * must not survive reset() and leak out through get_value()/get_bps()
	 * before the ~2s staleness window would otherwise have cleared it. */
	struct ltntstools_throughput_s s;
	memset(&s, 0, sizeof(s));
	s.Bps_window = 2000000;
	s.Bps_last_update = time(NULL) - 10;

	uint8_t buf[1];
	ltntstools_throughput_write(&s, buf, sizeof(buf));
	CHECK(s.Bps == 2000000); /* sanity: rollover happened */

	ltntstools_throughput_reset(&s);

	CHECK(s.Bps == 0);
	CHECK(s.Bps_window == 0);
	CHECK(ltntstools_throughput_get_value(&s) == 0);
	CHECK(ltntstools_throughput_get_bps(&s) == 0);
}

int main(void)
{
	test_write_value_first_call_only_fills_window();
	test_write_value_rollover_moves_window_to_bps();
	test_write_value_accumulates_within_same_second();

	test_write_rollover_computes_mbps_with_8x();
	test_write_accumulates_within_same_second();

	test_get_value_returns_bps_when_fresh();
	test_get_bps_multiplies_value_by_8();
	test_get_mbps_returns_stored_field_when_fresh();
	test_expire_clears_stats_after_stale_period();
	test_expire_leaves_stats_when_within_window();
	test_get_bps_reflects_expiry_too();

	test_reset_zeroes_byte_count_and_mbps();
	test_reset_clears_bps_so_get_value_and_get_bps_report_zero();

	if (g_failures == 0) {
		printf("PASS: all throughput tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d throughput test(s) failed\n", g_failures);
	return 1;
}
