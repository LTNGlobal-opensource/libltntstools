/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/throughput_hires.c /
 * src/libltntstools/throughput_hires.h. Builds against
 * ../src/throughput_hires.c only (xorg-list.h is header-only).
 *
 * THREE REAL BUGS were found in throughput_hires_minmaxavg_i64() while
 * writing these tests, each confirmed with a standalone repro, and have
 * been FIXED in src/throughput_hires.c as part of this change:
 *
 * 1. Wrong early-exit variable: the scan loop (newest items first) broke
 *    out via `if (e->timestamp < end) break;`. Since `end` defaults to
 *    "now", this is true for essentially every real item (anything written
 *    even a few microseconds ago), so the loop broke after examining only
 *    the single newest item -- min/max/avg reflected just that one entry,
 *    silently ignoring every older item still within the window. The
 *    sibling throughput_hires_sumtotal_i64() has the same early-exit
 *    optimization but correctly compares against `begin`. Fixed to compare
 *    against `begin` here too. Confirmed via repro: 5 items with values
 *    10/20/30/40/50 produced min=50 max=50 (only the newest item) before
 *    the fix, min=10 max=50 after.
 *
 * 2. Average accumulator seeded at -1: `*vavg = -1;` before the loop was
 *    meant as a "no data" sentinel, but the loop then does
 *    `*vavg += e->value_i64;` directly into that same variable, so the
 *    running sum started at -1 instead of 0 -- every computed average was
 *    off by (roughly) -1/N. Confirmed via repro: a single item with value
 *    100 produced avg=99 before the fix. Fixed by seeding the accumulator
 *    at 0 (the post-loop `else *vavg = -1;` path still correctly reports
 *    -1 when zero items match, so "no data" signaling is unaffected).
 *
 * 3. Bad *vmax sentinel for negative values: `*vmax = -1;` combined with
 *    `if (value > *vmax) *vmax = value;` means any dataset whose true
 *    maximum is <= -1 (e.g. all-negative values) never updates vmax away
 *    from the -1 sentinel, silently reporting the wrong (too-high) max.
 *    Confirmed via repro: values {-50,-10,-100} reported max=-1 instead of
 *    the correct -10. Fixed by seeding *vmax at INT64_MIN, mirroring how
 *    *vmin is already seeded at a value (1<<62) no real value can exceed.
 *
 * throughput_hires_sumtotal_i64() was checked against the same failure
 * modes and found already correct (it seeds its running total at 0 and
 * compares its early-exit against `begin`).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "libltntstools/throughput_hires.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

static struct timeval tv_add_us(struct timeval base, int64_t us)
{
	int64_t total = (int64_t)base.tv_sec * 1000000LL + base.tv_usec + us;
	struct timeval r;
	r.tv_sec = total / 1000000LL;
	r.tv_usec = total % 1000000LL;
	if (r.tv_usec < 0) {
		r.tv_usec += 1000000;
		r.tv_sec -= 1;
	}
	return r;
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	void *hdl = NULL;
	CHECK(throughput_hires_alloc(&hdl, 1000) == 0);
	CHECK(hdl != NULL);
	throughput_hires_free(hdl);
}

static void test_free_with_populated_lists_no_crash(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 10);
	for (int i = 0; i < 20; i++) {
		throughput_hires_write_i64(hdl, 0, i, NULL);
	}
	throughput_hires_free(hdl);
	CHECK(1);
}

static void test_write_grows_pool_when_exhausted(void)
{
	/* Tiny initial pool (2 items); writing well beyond that must not lose
	 * data -- the free list auto-grows by 64 when exhausted. */
	void *hdl;
	throughput_hires_alloc(&hdl, 2);

	struct timeval base;
	gettimeofday(&base, NULL);

	int64_t expectedSum = 0;
	for (int i = 0; i < 100; i++) {
		struct timeval ts = tv_add_us(base, -1000 * (100 - i));
		throughput_hires_write_i64(hdl, 0, i, &ts);
		expectedSum += i;
	}

	struct timeval from = tv_add_us(base, -1000 * 200);
	struct timeval to = tv_add_us(base, 1000);
	int64_t sum = throughput_hires_sumtotal_i64(hdl, 0, &from, &to);
	CHECK(sum == expectedSum);

	throughput_hires_free(hdl);
}

/* -------- sumtotal_i64 -------- */

static void test_write_and_sumtotal_single_item(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval ts = { .tv_sec = 1000, .tv_usec = 0 };
	throughput_hires_write_i64(hdl, 0, 42, &ts);

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 42);

	throughput_hires_free(hdl);
}

static void test_sumtotal_multiple_items_same_channel(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval base = { .tv_sec = 1000, .tv_usec = 0 };
	for (int i = 0; i < 5; i++) {
		struct timeval ts = tv_add_us(base, i * 100000);
		throughput_hires_write_i64(hdl, 0, (i + 1) * 10, &ts);
	}

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1002, .tv_usec = 0 };
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 150); /* 10+20+30+40+50 */

	throughput_hires_free(hdl);
}

static void test_sumtotal_channel_filtering(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval ts = { .tv_sec = 1000, .tv_usec = 0 };
	throughput_hires_write_i64(hdl, 0, 100, &ts);
	throughput_hires_write_i64(hdl, 1, 999, &ts);
	throughput_hires_write_i64(hdl, 0, 50, &ts);

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 150);
	CHECK(throughput_hires_sumtotal_i64(hdl, 1, &from, &to) == 999);
	CHECK(throughput_hires_sumtotal_i64(hdl, 2, &from, &to) == 0); /* never written */

	throughput_hires_free(hdl);
}

static void test_sumtotal_time_window_excludes_out_of_range(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval inWindow = { .tv_sec = 1000, .tv_usec = 0 };
	struct timeval before = { .tv_sec = 900, .tv_usec = 0 };
	struct timeval after = { .tv_sec = 1100, .tv_usec = 0 };

	throughput_hires_write_i64(hdl, 0, 1, &before);
	throughput_hires_write_i64(hdl, 0, 100, &inWindow);
	throughput_hires_write_i64(hdl, 0, 1, &after);

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 100);

	throughput_hires_free(hdl);
}

static void test_sumtotal_inclusive_boundaries(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval from = { .tv_sec = 1000, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };

	throughput_hires_write_i64(hdl, 0, 7, &from); /* exactly at `from` */
	throughput_hires_write_i64(hdl, 0, 9, &to);   /* exactly at `to` */

	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 16);

	throughput_hires_free(hdl);
}

static void test_sumtotal_no_matching_items_returns_zero(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval from = { .tv_sec = 1000, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 0);

	throughput_hires_free(hdl);
}

/* -------- minmaxavg_i64 -------- */

static void test_minmaxavg_basic(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval base = { .tv_sec = 1000, .tv_usec = 0 };
	for (int i = 0; i < 5; i++) {
		struct timeval ts = tv_add_us(base, i * 100000);
		throughput_hires_write_i64(hdl, 0, (i + 1) * 10, &ts); /* 10,20,30,40,50 */
	}

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1002, .tv_usec = 0 };
	int64_t vmin, vmax, vavg;
	CHECK(throughput_hires_minmaxavg_i64(hdl, 0, &from, &to, &vmin, &vmax, &vavg) == 0);
	CHECK(vmin == 10);
	CHECK(vmax == 50);
	CHECK(vavg == 30);

	throughput_hires_free(hdl);
}

static void test_minmaxavg_single_item_exact_average(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval ts = { .tv_sec = 1000, .tv_usec = 0 };
	throughput_hires_write_i64(hdl, 0, 100, &ts);

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	int64_t vmin, vmax, vavg;
	throughput_hires_minmaxavg_i64(hdl, 0, &from, &to, &vmin, &vmax, &vavg);
	CHECK(vmin == 100);
	CHECK(vmax == 100);
	CHECK(vavg == 100);

	throughput_hires_free(hdl);
}

static void test_minmaxavg_all_negative_values(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval base = { .tv_sec = 1000, .tv_usec = 0 };
	int values[3] = { -50, -10, -100 };
	for (int i = 0; i < 3; i++) {
		struct timeval ts = tv_add_us(base, i * 1000);
		throughput_hires_write_i64(hdl, 0, values[i], &ts);
	}

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	int64_t vmin, vmax, vavg;
	throughput_hires_minmaxavg_i64(hdl, 0, &from, &to, &vmin, &vmax, &vavg);
	CHECK(vmin == -100);
	CHECK(vmax == -10);
	CHECK(vavg == -53); /* (-50-10-100)/3 = -53.33, truncates toward 0 */

	throughput_hires_free(hdl);
}

static void test_minmaxavg_no_items_returns_sentinels(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval from = { .tv_sec = 1000, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	int64_t vmin, vmax, vavg;
	CHECK(throughput_hires_minmaxavg_i64(hdl, 0, &from, &to, &vmin, &vmax, &vavg) == 0);
	CHECK(vavg == -1); /* documented "no data" sentinel */

	throughput_hires_free(hdl);
}

static void test_minmaxavg_channel_filtering(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval ts = { .tv_sec = 1000, .tv_usec = 0 };
	throughput_hires_write_i64(hdl, 5, 1000, &ts);
	throughput_hires_write_i64(hdl, 7, 5, &ts);
	throughput_hires_write_i64(hdl, 7, 15, &ts);

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	int64_t vmin, vmax, vavg;
	throughput_hires_minmaxavg_i64(hdl, 7, &from, &to, &vmin, &vmax, &vavg);
	CHECK(vmin == 5);
	CHECK(vmax == 15);
	CHECK(vavg == 10);

	throughput_hires_free(hdl);
}

static void test_minmaxavg_time_window_excludes_out_of_range(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval before = { .tv_sec = 900, .tv_usec = 0 };
	struct timeval inWindow = { .tv_sec = 1000, .tv_usec = 0 };
	struct timeval after = { .tv_sec = 1100, .tv_usec = 0 };

	throughput_hires_write_i64(hdl, 0, 9999, &before);
	throughput_hires_write_i64(hdl, 0, 42, &inWindow);
	throughput_hires_write_i64(hdl, 0, 9999, &after);

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	int64_t vmin, vmax, vavg;
	throughput_hires_minmaxavg_i64(hdl, 0, &from, &to, &vmin, &vmax, &vavg);
	CHECK(vmin == 42);
	CHECK(vmax == 42);
	CHECK(vavg == 42);

	throughput_hires_free(hdl);
}

/* -------- expire -------- */

static void test_expire_removes_old_items_and_returns_count(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval old1 = { .tv_sec = 100, .tv_usec = 0 };
	struct timeval old2 = { .tv_sec = 200, .tv_usec = 0 };
	struct timeval recent = { .tv_sec = 1000, .tv_usec = 0 };

	throughput_hires_write_i64(hdl, 0, 1, &old1);
	throughput_hires_write_i64(hdl, 0, 2, &old2);
	throughput_hires_write_i64(hdl, 0, 3, &recent);

	struct timeval threshold = { .tv_sec = 500, .tv_usec = 0 };
	int expired = throughput_hires_expire(hdl, &threshold);
	CHECK(expired == 2);

	throughput_hires_free(hdl);
}

static void test_expire_keeps_recent_items(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval recent = { .tv_sec = 1000, .tv_usec = 0 };
	throughput_hires_write_i64(hdl, 0, 42, &recent);

	struct timeval threshold = { .tv_sec = 500, .tv_usec = 0 };
	int expired = throughput_hires_expire(hdl, &threshold);
	CHECK(expired == 0);

	struct timeval from = { .tv_sec = 999, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 1001, .tv_usec = 0 };
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 42);

	throughput_hires_free(hdl);
}

static void test_expired_items_excluded_from_sumtotal(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	struct timeval old = { .tv_sec = 100, .tv_usec = 0 };
	struct timeval recent = { .tv_sec = 1000, .tv_usec = 0 };
	throughput_hires_write_i64(hdl, 0, 1000, &old);
	throughput_hires_write_i64(hdl, 0, 5, &recent);

	struct timeval threshold = { .tv_sec = 500, .tv_usec = 0 };
	throughput_hires_expire(hdl, &threshold);

	/* Wide window that would have included the expired item too, if it
	 * hadn't been recycled back to the free list. */
	struct timeval from = { .tv_sec = 0, .tv_usec = 0 };
	struct timeval to = { .tv_sec = 2000, .tv_usec = 0 };
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 5);

	throughput_hires_free(hdl);
}

/* -------- NULL ts uses walltime -------- */

static void test_write_null_ts_uses_walltime(void)
{
	void *hdl;
	throughput_hires_alloc(&hdl, 100);

	throughput_hires_write_i64(hdl, 0, 77, NULL);

	struct timeval now;
	gettimeofday(&now, NULL);
	struct timeval from = tv_add_us(now, -2 * 1000000);
	struct timeval to = tv_add_us(now, 2 * 1000000);
	CHECK(throughput_hires_sumtotal_i64(hdl, 0, &from, &to) == 77);

	throughput_hires_free(hdl);
}

int main(void)
{
	test_alloc_free_basic();
	test_free_with_populated_lists_no_crash();
	test_write_grows_pool_when_exhausted();

	test_write_and_sumtotal_single_item();
	test_sumtotal_multiple_items_same_channel();
	test_sumtotal_channel_filtering();
	test_sumtotal_time_window_excludes_out_of_range();
	test_sumtotal_inclusive_boundaries();
	test_sumtotal_no_matching_items_returns_zero();

	test_minmaxavg_basic();
	test_minmaxavg_single_item_exact_average();
	test_minmaxavg_all_negative_values();
	test_minmaxavg_no_items_returns_sentinels();
	test_minmaxavg_channel_filtering();
	test_minmaxavg_time_window_excludes_out_of_range();

	test_expire_removes_old_items_and_returns_count();
	test_expire_keeps_recent_items();
	test_expired_items_excluded_from_sumtotal();

	test_write_null_ts_uses_walltime();

	if (g_failures == 0) {
		printf("PASS: all throughput_hires tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d throughput_hires test(s) failed\n", g_failures);
	return 1;
}
