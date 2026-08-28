/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/clocks.c / src/libltntstools/clocks.h.
 * Builds against ../src/clocks.c only -- the module has no other
 * dependencies (it pulls in libltntstools/ltntstools.h purely for
 * declarations, memset/gettimeofday/snprintf come from the C library).
 *
 * struct ltntstools_clock_s and struct ltntstools_corrected_clock_s are
 * fully public (not opaque), so several tests below poke fields directly
 * to build a deterministic fixture (eg. rigging clk->establishedWalltime
 * to a known point in the past) rather than relying on real elapsed time,
 * which would make the test flaky.
 *
 * NOTABLE BEHAVIOUR PINNED HERE (not bugs to fix, just documented so a
 * future change that alters them is a deliberate decision):
 *  - ltntstools_corrected_clock_update()'s "first call" branch
 *    (`if (ctx->initialized == 0)`) is dead code: ltntstools_corrected_clock_init()
 *    always sets initialized=1, and update() itself refuses to run at all
 *    when !ctx->initialized. So the very first real update() is computed
 *    as a delta from lastTickValue==0, not seeded directly -- see
 *    test_corrected_clock_first_update_is_delta_from_zero().
 *  - ltntstools_corrected_clock_unwrapped() returns ctx->unwrapped, which
 *    is allowed to dip backward (eg. on B-frame reordering); the
 *    ctx->correctedClk field that holds a monotonic view internally is
 *    never exposed by any public accessor. See
 *    test_corrected_clock_reorder_holds_correctedClk_but_unwrapped_dips().
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>

#include "libltntstools/ltntstools.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

#define CHECK_EQ_I64(a, b) \
	do { \
		int64_t _a = (int64_t)(a), _b = (int64_t)(b); \
		if (_a != _b) { \
			fprintf(stderr, "FAIL: %s:%d: %s (%" PRId64 ") != %s (%" PRId64 ")\n", \
				__FILE__, __LINE__, #a, _a, #b, _b); \
			g_failures++; \
		} \
	} while (0)

#define CHECK_EQ_U64(a, b) \
	do { \
		uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); \
		if (_a != _b) { \
			fprintf(stderr, "FAIL: %s:%d: %s (%" PRIu64 ") != %s (%" PRIu64 ")\n", \
				__FILE__, __LINE__, #a, _a, #b, _b); \
			g_failures++; \
		} \
	} while (0)

/* ---------------------------------------------------------------------
 * Metadata / type name plumbing
 * --------------------------------------------------------------------- */

static void test_clock_type_name(void)
{
	CHECK(strcmp(ltntstools_clock_type_name(ltntstools_CLOCK_TYPE_PCR), "PCR") == 0);
	CHECK(strcmp(ltntstools_clock_type_name(ltntstools_CLOCK_TYPE_PTS), "PTS") == 0);
	CHECK(strcmp(ltntstools_clock_type_name(ltntstools_CLOCK_TYPE_DTS), "DTS") == 0);
	CHECK(strcmp(ltntstools_clock_type_name(ltntstools_CLOCK_TYPE_RTP), "RTP") == 0);
	CHECK(strcmp(ltntstools_clock_type_name(ltntstools_CLOCK_TYPE_NTP), "NTP") == 0);
	CHECK(strcmp(ltntstools_clock_type_name(ltntstools_CLOCK_TYPE_UNKNOWN), "UNKNOWN") == 0);
	CHECK(strcmp(ltntstools_clock_type_name((enum ltntstools_clock_type_e)999), "UNKNOWN") == 0);
}

static void test_clock_initialize(void)
{
	struct ltntstools_clock_s clk;
	memset(&clk, 0xAA, sizeof(clk)); /* poison, to prove initialize() clears it */

	ltntstools_clock_initialize(&clk);

	CHECK(ltntstools_clock_get_type(&clk) == ltntstools_CLOCK_TYPE_UNKNOWN);
	CHECK(strcmp(ltntstools_clock_get_name(&clk), "UNKNOWN") == 0);
	CHECK(ltntstools_clock_is_established_timebase(&clk) == 0);
	CHECK(ltntstools_clock_is_established_wallclock(&clk) == 0);
	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 0);
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 0);
	CHECK_EQ_I64(ltntstools_clock_get_wrap_occurences(&clk), 0);
	CHECK_EQ_U64(ltntstools_clock_get_backward_jump_under_500ms_count(&clk), 0);
	CHECK_EQ_U64(ltntstools_clock_get_forward_jump_over_200ms_count(&clk), 0);
	CHECK_EQ_U64(ltntstools_clock_get_discontinuity_count(&clk), 0);
}

/* set_metadata() auto-establishes the well known timebases, and falls back
 * to the type name whenever the caller doesn't supply one. */
static void test_clock_set_metadata_autotimebase(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PCR, NULL);
	CHECK(ltntstools_clock_is_established_timebase(&clk) == 1);
	CHECK_EQ_I64(clk.ticks_per_second, 27000000);
	CHECK_EQ_I64(clk.clockWrapValue, MAX_SCR_VALUE);
	CHECK(strcmp(ltntstools_clock_get_name(&clk), "PCR") == 0);

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);
	CHECK(ltntstools_clock_is_established_timebase(&clk) == 1);
	CHECK_EQ_I64(clk.ticks_per_second, 90000);
	CHECK_EQ_I64(clk.clockWrapValue, MAX_PTS_VALUE);

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_DTS, NULL);
	CHECK_EQ_I64(clk.ticks_per_second, 90000);

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_RTP, NULL);
	CHECK_EQ_I64(clk.ticks_per_second, 90000);

	/* NTP and UNKNOWN do not auto-establish a timebase. */
	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_NTP, NULL);
	CHECK(ltntstools_clock_is_established_timebase(&clk) == 0);

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_UNKNOWN, NULL);
	CHECK(ltntstools_clock_is_established_timebase(&clk) == 0);
}

static void test_clock_set_metadata_name_fallback(void)
{
	struct ltntstools_clock_s clk;

	/* Caller supplied name is preserved verbatim. */
	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PCR, "video-pcr");
	CHECK(strcmp(ltntstools_clock_get_name(&clk), "video-pcr") == 0);

	/* NULL name falls back to the type name. */
	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);
	CHECK(strcmp(ltntstools_clock_get_name(&clk), "PTS") == 0);

	/* Empty string name also falls back to the type name. */
	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_DTS, "");
	CHECK(strcmp(ltntstools_clock_get_name(&clk), "DTS") == 0);

	/* get_name()/get_type() on a NULL context are safe and return the
	 * UNKNOWN defaults rather than crashing. */
	CHECK(strcmp(ltntstools_clock_get_name(NULL), "UNKNOWN") == 0);
	CHECK(ltntstools_clock_get_type(NULL) == ltntstools_CLOCK_TYPE_UNKNOWN);
}

/* ---------------------------------------------------------------------
 * Timebase / wallclock establishment
 * --------------------------------------------------------------------- */

static void test_clock_establish_timebase_wrap_values(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_establish_timebase(&clk, 90000);
	CHECK_EQ_I64(clk.clockWrapValue, MAX_PTS_VALUE);

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_establish_timebase(&clk, 27000000);
	CHECK_EQ_I64(clk.clockWrapValue, MAX_SCR_VALUE);

	/* Anything else defaults to a 32-bit wrap (eg. RTMP-style timestamps). */
	ltntstools_clock_initialize(&clk);
	ltntstools_clock_establish_timebase(&clk, 1000);
	CHECK_EQ_I64(clk.clockWrapValue, (INT64_C(1) << 32));
}

static void test_clock_establish_wallclock(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);

	CHECK(ltntstools_clock_is_established_wallclock(&clk) == 0);
	ltntstools_clock_establish_wallclock(&clk, 12345);
	CHECK(ltntstools_clock_is_established_wallclock(&clk) == 1);
	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 12345);
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 12345);
	CHECK_EQ_I64(clk.monotonicReference_ticks, 12345);
	CHECK_EQ_I64(clk.establishedTime_ticks, 12345);
}

/* ---------------------------------------------------------------------
 * set_ticks() -- forward jumps, backward jumps, wrap detection
 * --------------------------------------------------------------------- */

static void test_set_ticks_forward_progress(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_establish_timebase(&clk, 1000); /* small numbers, easy to reason about */
	ltntstools_clock_establish_wallclock(&clk, 1000);

	/* Small forward step: below the (ticks_per_second/5 == 200) threshold, no jump flagged. */
	ltntstools_clock_set_ticks(&clk, 1100);
	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 1100);
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 1100);
	CHECK_EQ_U64(ltntstools_clock_get_forward_jump_over_200ms_count(&clk), 0);

	/* Large forward step: exceeds the threshold, gets flagged, but still
	 * accumulates into the monotonic timeline. */
	ltntstools_clock_set_ticks(&clk, 1100 + 300);
	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 1400);
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 1400);
	CHECK_EQ_U64(ltntstools_clock_get_forward_jump_over_200ms_count(&clk), 1);
}

static void test_set_ticks_backward_jump_boundary(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_establish_timebase(&clk, 1000); /* threshold ticks_per_second/2 == 500 */

	/* Strictly less than the threshold: counted. */
	ltntstools_clock_establish_wallclock(&clk, 1000);
	ltntstools_clock_set_ticks(&clk, 501); /* delta 499 < 500 */
	CHECK_EQ_U64(ltntstools_clock_get_backward_jump_under_500ms_count(&clk), 1);
	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 501);
	/* Backward jumps (that aren't wraps) never move the monotonic timeline. */
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 1000);

	/* Exactly at the threshold: NOT counted (condition is strict '<'). */
	ltntstools_clock_establish_wallclock(&clk, 1000);
	ltntstools_clock_set_ticks(&clk, 500); /* delta 500, not < 500 */
	CHECK_EQ_U64(ltntstools_clock_get_backward_jump_under_500ms_count(&clk), 0);
}

static void test_set_ticks_wrap_detection(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL); /* 90kHz, MAX_PTS_VALUE wrap */
	ltntstools_clock_establish_wallclock(&clk, MAX_PTS_VALUE - 1000);

	/* A tick value under current/50 looks like the 33-bit PTS counter wrapping,
	 * not a random backward jump -- the monotonic timeline should keep climbing. */
	ltntstools_clock_set_ticks(&clk, 500);

	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 500);
	CHECK_EQ_I64(ltntstools_clock_get_wrap_occurences(&clk), 1);
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), MAX_PTS_VALUE + 500);
	/* A genuine wrap must not also be counted as a plain backward jump. */
	CHECK_EQ_U64(ltntstools_clock_get_backward_jump_under_500ms_count(&clk), 0);
}

static void test_clock_compute_delta(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_establish_timebase(&clk, 1000);
	clk.clockWrapValue = 1000; /* rig a small, easy to hand-check wrap value */

	/* No wrap: plain subtraction. */
	CHECK_EQ_I64(ltntstools_clock_compute_delta(&clk, 800, 200), 600);

	/* Wrapped: distance forward across the wrap boundary. */
	CHECK_EQ_I64(ltntstools_clock_compute_delta(&clk, 100, 900), 200);
}

/* ---------------------------------------------------------------------
 * mark_discontinuity() -- next set_ticks() call rebases instead of
 * counting anomalies.
 * --------------------------------------------------------------------- */

static void test_mark_discontinuity_rebases_next_set_ticks(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);
	ltntstools_clock_establish_wallclock(&clk, 1000);

	/* Rack up a forward jump and a wrap first, so we can tell what
	 * mark_discontinuity() does and doesn't reset. */
	ltntstools_clock_set_ticks(&clk, 1000 + 50000); /* forward jump: threshold is 90000/5 == 18000 */
	CHECK_EQ_U64(ltntstools_clock_get_forward_jump_over_200ms_count(&clk), 1);

	ltntstools_clock_set_ticks(&clk, 10); /* current/50 is large, 10 looks like a wrap */
	CHECK_EQ_I64(ltntstools_clock_get_wrap_occurences(&clk), 1);

	CHECK_EQ_U64(ltntstools_clock_get_discontinuity_count(&clk), 0);
	ltntstools_clock_mark_discontinuity(&clk);
	CHECK_EQ_U64(ltntstools_clock_get_discontinuity_count(&clk), 1);

	/* The next set_ticks(), however far away, must be treated as a clean
	 * rebase: no wrap/backward/forward counters move, and the clock
	 * (both raw and monotonic) jumps straight to the new value. */
	ltntstools_clock_set_ticks(&clk, 999999);

	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 999999);
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 999999);
	CHECK_EQ_I64(ltntstools_clock_get_wrap_occurences(&clk), 0); /* reset by the rebase */
	CHECK_EQ_U64(ltntstools_clock_get_forward_jump_over_200ms_count(&clk), 1); /* untouched */
	CHECK_EQ_U64(ltntstools_clock_get_discontinuity_count(&clk), 1); /* only mark_discontinuity() bumps this */
	CHECK(ltntstools_clock_is_established_wallclock(&clk) == 1);

	/* Discontinuity handling is one-shot: the call after the rebase behaves normally again. */
	ltntstools_clock_set_ticks(&clk, 999999 + 100); /* well under the 18000 forward-jump threshold */
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 999999 + 100);
	CHECK_EQ_U64(ltntstools_clock_get_forward_jump_over_200ms_count(&clk), 1);
}

/* ---------------------------------------------------------------------
 * add_ticks() -- relative adjustment with wrap/unwrap handling
 * --------------------------------------------------------------------- */

static void test_add_ticks_forward_wrap(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);
	ltntstools_clock_establish_wallclock(&clk, MAX_PTS_VALUE - 10);

	ltntstools_clock_add_ticks(&clk, 50);

	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), 40); /* (MAX_PTS_VALUE - 10 + 50) - MAX_PTS_VALUE */
	CHECK_EQ_I64(ltntstools_clock_get_wrap_occurences(&clk), 1);
	/* The monotonic timeline is never folded back down across a wrap. */
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), MAX_PTS_VALUE + 40);
}

static void test_add_ticks_negative_wrap(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);
	ltntstools_clock_establish_wallclock(&clk, 5);

	ltntstools_clock_add_ticks(&clk, -10);

	CHECK_EQ_I64(ltntstools_clock_get_ticks(&clk), MAX_PTS_VALUE - 5);
	/* Negative ticks don't advance the monotonic timeline at all. */
	CHECK_EQ_I64(ltntstools_clock_get_monotonic_ticks(&clk), 5);
	/* wrapOccurences is never allowed to go negative when there was nothing to undo. */
	CHECK_EQ_I64(ltntstools_clock_get_wrap_occurences(&clk), 0);
}

/* ---------------------------------------------------------------------
 * get_drift_us()/get_drift_ms() -- rigged against a known-past
 * establishedWalltime so the assertions don't depend on real sleeps.
 * --------------------------------------------------------------------- */

static void test_get_drift_before_wallclock_established(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);

	/* No wallclock established yet -- must short circuit to 0, not read
	 * whatever garbage is in establishedWalltime. */
	CHECK_EQ_I64(ltntstools_clock_get_drift_us(&clk), 0);
	CHECK_EQ_I64(ltntstools_clock_get_drift_ms(&clk), 0);
}

static void test_get_drift_us_computed(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL); /* 90kHz */
	ltntstools_clock_establish_wallclock(&clk, 0);

	/* Rig establishedWalltime to exactly 1 real second ago, and pretend
	 * exactly 1 second (90000 ticks @ 90kHz) of timebase has elapsed too.
	 * If the two agree, drift should be ~0 -- the only slack is the tiny
	 * amount of real time this test itself burns between the two
	 * gettimeofday() calls, which can only push drift_us slightly
	 * negative (tickTime is fixed, elapsedWT can only grow), never positive.
	 */
	gettimeofday(&clk.establishedWalltime, NULL);
	clk.establishedWalltime.tv_sec -= 1;
	clk.monotonicTime_ticks = 90000;
	clk.establishedTime_ticks = 0;

	int64_t drift_us = ltntstools_clock_get_drift_us(&clk);

	CHECK(drift_us <= 0);
	CHECK(drift_us > -20000); /* generous 20ms slack for slow/loaded test hosts */

	/* hwm/lwm/max bookkeeping: starts at 0, and since drift_us <= 0 here,
	 * only the low-water-mark should move. */
	CHECK_EQ_I64(clk.drift_us_hwm, 0);
	CHECK_EQ_I64(clk.drift_us_lwm, drift_us);
	CHECK_EQ_I64(clk.drift_us_max, -drift_us);
}

static void test_get_drift_ms_computed(void)
{
	struct ltntstools_clock_s clk;

	ltntstools_clock_initialize(&clk);
	ltntstools_clock_set_metadata(&clk, ltntstools_CLOCK_TYPE_PTS, NULL);
	ltntstools_clock_establish_wallclock(&clk, 0);

	gettimeofday(&clk.establishedWalltime, NULL);
	clk.establishedWalltime.tv_sec -= 2;
	clk.monotonicTime_ticks = 2 * 90000;
	clk.establishedTime_ticks = 0;

	int64_t drift_ms = ltntstools_clock_get_drift_ms(&clk);
	CHECK(drift_ms <= 0);
	CHECK(drift_ms > -50); /* generous slack, see test_get_drift_us_computed() */
}

/* ---------------------------------------------------------------------
 * compare() / compare_us() / compare_ms() -- cross-timebase comparison
 * --------------------------------------------------------------------- */

static void test_clock_compare(void)
{
	struct ltntstools_clock_s a, b, unestablished;

	ltntstools_clock_initialize(&a);
	ltntstools_clock_set_metadata(&a, ltntstools_CLOCK_TYPE_PTS, NULL); /* 90kHz */
	a.monotonicTime_ticks = 90000; /* 1.0s */

	ltntstools_clock_initialize(&b);
	ltntstools_clock_set_metadata(&b, ltntstools_CLOCK_TYPE_PCR, NULL); /* 27MHz */
	b.monotonicTime_ticks = 27000000; /* 1.0s */

	CHECK_EQ_I64(ltntstools_clock_compare_us(&a, &b), 0);
	CHECK_EQ_I64(ltntstools_clock_compare_ms(&a, &b), 0);

	/* Push clock A 100ms ahead and re-check the converted delta. */
	a.monotonicTime_ticks = 99000; /* 1.1s @ 90kHz */
	CHECK_EQ_I64(ltntstools_clock_compare_us(&a, &b), 100000);
	CHECK_EQ_I64(ltntstools_clock_compare_ms(&a, &b), 100);
	/* And the reverse comparison is the negated delta. */
	CHECK_EQ_I64(ltntstools_clock_compare_us(&b, &a), -100000);

	/* NULL-safety: no crash, defined return of 0. */
	CHECK_EQ_I64(ltntstools_clock_compare(NULL, &b, 1000000), 0);
	CHECK_EQ_I64(ltntstools_clock_compare(&a, NULL, 1000000), 0);
	CHECK_EQ_I64(ltntstools_clock_compare(&a, &b, 0), 0);

	/* A clock whose timebase was never established (ticks_per_second == 0)
	 * must not be divided by. */
	ltntstools_clock_initialize(&unestablished);
	CHECK_EQ_I64(ltntstools_clock_compare(&a, &unestablished, 1000000), 0);
}

/* ---------------------------------------------------------------------
 * corrected_clock -- 33-bit PTS unwrap/wrap tracking
 * --------------------------------------------------------------------- */

static void test_corrected_clock_init(void)
{
	struct ltntstools_corrected_clock_s ctx;
	memset(&ctx, 0xAA, sizeof(ctx));

	CHECK(ltntstools_corrected_clock_init(&ctx, 90000) == 0);
	CHECK(ctx.initialized == 1);
	CHECK_EQ_U64(ctx.clk_bits, 33);
	CHECK_EQ_U64(ctx.clkMaxTicks, (1ULL << 33));
	CHECK_EQ_U64(ctx.clk_mod, (1ULL << 33));
	CHECK_EQ_U64(ctx.clk_mask, (1ULL << 33) - 1);
	CHECK_EQ_U64(ctx.clk_half, (1ULL << 32));
	CHECK_EQ_U64(ctx.lastTickValue, 0);
	CHECK_EQ_I64(ctx.unwrapped, 0);
	CHECK_EQ_U64(ctx.correctedClk, 0);

	/* Only 90kHz (33-bit PTS) is supported today. */
	memset(&ctx, 0, sizeof(ctx));
	CHECK(ltntstools_corrected_clock_init(&ctx, 27000000) < 0);
}

static void test_corrected_clock_null_and_uninitialized_safety(void)
{
	struct ltntstools_corrected_clock_s ctx;
	memset(&ctx, 0, sizeof(ctx)); /* never _init()'d: initialized == 0 */

	CHECK(ltntstools_corrected_clock_update(NULL, 1000) < 0);
	CHECK(ltntstools_corrected_clock_update(&ctx, 1000) < 0);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(NULL), 0);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), 0);
}

/* Documents the dead "first call" seeding branch: since _init() already
 * sets initialized=1, the real first update() is computed as a delta
 * away from lastTickValue==0, exactly like every later call. */
static void test_corrected_clock_first_update_is_delta_from_zero(void)
{
	struct ltntstools_corrected_clock_s ctx;
	ltntstools_corrected_clock_init(&ctx, 90000);

	CHECK(ltntstools_corrected_clock_update(&ctx, 1000) == 0);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), 1000);

	CHECK(ltntstools_corrected_clock_update(&ctx, 4000) == 0);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), 4000);
}

/* A small backward step (eg. a B-frame PTS earlier than the preceding
 * P-frame) is within JITTER_MAX and must not be treated as an error, but
 * the publicly visible unwrapped() value is allowed to dip -- only the
 * internal (unexposed) correctedClk field actually holds the high water mark. */
static void test_corrected_clock_reorder_holds_correctedClk_but_unwrapped_dips(void)
{
	struct ltntstools_corrected_clock_s ctx;
	ltntstools_corrected_clock_init(&ctx, 90000);

	ltntstools_corrected_clock_update(&ctx, 1000);
	ltntstools_corrected_clock_update(&ctx, 4000);
	CHECK_EQ_U64(ctx.correctedClk, 4000);

	int ret = ltntstools_corrected_clock_update(&ctx, 3000); /* 1000 ticks backward, well under JITTER_MAX */
	CHECK(ret == 0);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), 3000); /* dipped */
	CHECK_EQ_U64(ctx.correctedClk, 4000); /* held internally, but never exposed to callers */
}

/* Crossing the 2^33 boundary must extend the unwrapped 64-bit timeline
 * rather than resetting it. */
static void test_corrected_clock_wraparound_extends_timeline(void)
{
	struct ltntstools_corrected_clock_s ctx;
	ltntstools_corrected_clock_init(&ctx, 90000);

	const uint64_t mask = ctx.clk_mask; /* 2^33 - 1 */

	/* Seed near the top of the 33-bit range. Because the first real
	 * update() is a delta from lastTickValue==0 (see
	 * test_corrected_clock_first_update_is_delta_from_zero()), a value
	 * this close to the top of the range looks like a small step
	 * *backward* across the wrap boundary, which trips the "massive
	 * correction" reset path and returns -1. */
	int ret = ltntstools_corrected_clock_update(&ctx, mask - 500);
	CHECK(ret == -1);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), mask - 500);

	/* Advance 300 ticks forward, still below the wrap. */
	CHECK(ltntstools_corrected_clock_update(&ctx, mask - 200) == 0);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), mask - 200);

	/* Now cross the wrap: raw ticks go from just under 2^33 back to a
	 * small value. The unwrapped timeline must keep climbing past
	 * clk_mod, not fall back to a small number. */
	CHECK(ltntstools_corrected_clock_update(&ctx, 100) == 0);
	uint64_t unwrapped = ltntstools_corrected_clock_unwrapped(&ctx);
	CHECK(unwrapped > mask);
	CHECK_EQ_U64(unwrapped, (mask - 200) + 301);
}

/* A backward jump beyond JITTER_MAX, but not large enough to make the
 * unwrapped timeline go negative, is held rather than corrected -- treated
 * as a reordering glitch, per the comment in ltntstools_corrected_clock_update(). */
static void test_corrected_clock_large_backward_jump_is_held(void)
{
	struct ltntstools_corrected_clock_s ctx;
	ltntstools_corrected_clock_init(&ctx, 90000);

	const uint64_t mask = ctx.clk_mask;
	ltntstools_corrected_clock_update(&ctx, mask - 500); /* ret==-1, see above; establishes a base */
	ltntstools_corrected_clock_update(&ctx, mask - 200);
	ltntstools_corrected_clock_update(&ctx, 100); /* wraps forward, unwrapped == mask + 101 */

	uint64_t before = ltntstools_corrected_clock_unwrapped(&ctx);
	uint64_t correctedClk_before = ctx.correctedClk;

	/* Jump back by 50000 ticks (> JITTER_MAX == 45000) without going negative:
	 * compute the exact raw tick value that yields a -50000 delta from the
	 * current lastTickValue. */
	uint64_t last = ctx.lastTickValue;
	uint64_t p33 = (uint64_t)(((int64_t)last - 50000) & (int64_t)mask);
	int ret = ltntstools_corrected_clock_update(&ctx, (int64_t)p33);

	CHECK(ret == 0); /* held, not treated as an error */
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), before - 50000);
	CHECK_EQ_U64(ctx.correctedClk, correctedClk_before); /* unchanged: held */
}

/* A backward jump large enough to drive the unwrapped timeline negative
 * forces a full reset back onto the raw tick value. */
static void test_corrected_clock_massive_backward_jump_resets(void)
{
	struct ltntstools_corrected_clock_s ctx;
	ltntstools_corrected_clock_init(&ctx, 90000);

	CHECK(ltntstools_corrected_clock_update(&ctx, 1000) == 0);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), 1000);

	/* Jump back by 2000 ticks from lastTickValue==1000: unwrapped would go to -1000. */
	uint64_t mask = ctx.clk_mask;
	uint64_t p33 = (uint64_t)(((int64_t)1000 - 2000) & (int64_t)mask);
	int ret = ltntstools_corrected_clock_update(&ctx, (int64_t)p33);

	CHECK(ret == -1);
	CHECK_EQ_U64(ltntstools_corrected_clock_unwrapped(&ctx), p33);
	CHECK_EQ_U64(ctx.correctedClk, p33);
}

int main(void)
{
	test_clock_type_name();
	test_clock_initialize();
	test_clock_set_metadata_autotimebase();
	test_clock_set_metadata_name_fallback();

	test_clock_establish_timebase_wrap_values();
	test_clock_establish_wallclock();

	test_set_ticks_forward_progress();
	test_set_ticks_backward_jump_boundary();
	test_set_ticks_wrap_detection();
	test_clock_compute_delta();

	test_mark_discontinuity_rebases_next_set_ticks();

	test_add_ticks_forward_wrap();
	test_add_ticks_negative_wrap();

	test_get_drift_before_wallclock_established();
	test_get_drift_us_computed();
	test_get_drift_ms_computed();

	test_clock_compare();

	test_corrected_clock_init();
	test_corrected_clock_null_and_uninitialized_safety();
	test_corrected_clock_first_update_is_delta_from_zero();
	test_corrected_clock_reorder_holds_correctedClk_but_unwrapped_dips();
	test_corrected_clock_wraparound_extends_timeline();
	test_corrected_clock_large_backward_jump_is_held();
	test_corrected_clock_massive_backward_jump_resets();

	if (g_failures == 0) {
		printf("PASS: all clocks tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d clocks test(s) failed\n", g_failures);
	return 1;
}
