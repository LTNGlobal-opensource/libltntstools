/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/vbv.c / src/libltntstools/vbv.h.
 * Builds against ../src/vbv.c plus its real (non-mocked) dependencies
 * ../src/pes.c (for ltn_pes_packet_has_PTS/DTS), ../src/crc32.c (pulled in
 * by pes.c), ../src/time.c (libltntstools_timespec_diff_ms) and
 * ../src/utils.c (ltnpthread_setname_np). Requires -lpthread since
 * ltntstools_vbv_alloc() spawns a background "virtual decoder" thread.
 *
 * SCOPE NOTE: the background thread only starts draining the VBV once
 * (dts_hwm - dts_lwm) exceeds a 60%-of-a-second "initial_cpb_removal_delay"
 * threshold, and its drain decisions are then paced against real
 * CLOCK_MONOTONIC time (50ms poll loop). Asserting drain/underflow/fullness
 * behaviour that depends on that thread actually running would make the
 * tests flaky and slow, so this file keeps every DTS delta below the
 * drain threshold (all test packets share/neighbor the same clock value)
 * and instead exercises the synchronous, deterministic parts of the API:
 * the lookup tables, profile helpers, ltntstools_vbv_write()'s overflow
 * and clock-reset logic (both run synchronously inside the caller's
 * thread), and ltntstools_vbv_get_fullness().
 *
 * framerateToNs()/framerateToUs()/framerateToTicks() have external linkage
 * in vbv.c but aren't declared in vbv.h; they're forward-declared here to
 * reach them directly. Their expected truncated values were confirmed
 * against the real implementation with a standalone repro (Python mirror
 * of the same double math) before being hard coded below.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/ltntstools.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* Not exposed via vbv.h, but globally linked from vbv.c. */
extern uint32_t framerateToNs(double framerate);
extern uint32_t framerateToUs(double framerate);
extern uint32_t framerateToTicks(double framerate);

static void makePkt(struct ltn_pes_packet_s *pkt, int64_t dts, uint32_t sizeBytes)
{
	memset(pkt, 0, sizeof(*pkt));
	pkt->PTS_DTS_flags = 1; /* DTS only */
	pkt->DTS = dts;
	pkt->rawBufferLengthBytes = sizeBytes;
}

/* -------- ltntstools_vbv_bitrate_lookup() -------- */

static void test_bitrate_lookup_known_values(void)
{
	CHECK(ltntstools_vbv_bitrate_lookup(VBV_CODEC_H264, 10) == (64 * 1000) / 8);
	CHECK(ltntstools_vbv_bitrate_lookup(VBV_CODEC_H264, 31) == (14000 * 1000) / 8);
	CHECK(ltntstools_vbv_bitrate_lookup(VBV_CODEC_H264, 51) == (240000 * 1000) / 8);
}

static void test_bitrate_lookup_unknown_returns_negative(void)
{
	CHECK(ltntstools_vbv_bitrate_lookup(VBV_CODEC_H264, 99) < 0);
	CHECK(ltntstools_vbv_bitrate_lookup(99 /* bogus codec */, 31) < 0);
}

/* -------- ltntstools_vbv_profile_defaults() -------- */

static void test_profile_defaults_valid(void)
{
	struct vbv_decoder_profile_s dp;
	memset(&dp, 0xff, sizeof(dp));

	CHECK(ltntstools_vbv_profile_defaults(&dp, VBV_CODEC_H264, 31, 29.97) == 0);
	CHECK(dp.vbv_buffer_size == (uint32_t)((14000 * 1000) / 8));
	CHECK(dp.framerate == 29.97);
}

static void test_profile_defaults_invalid_framerate(void)
{
	struct vbv_decoder_profile_s dp;
	CHECK(ltntstools_vbv_profile_defaults(&dp, VBV_CODEC_H264, 31, 27.5 /* not a valid framerate */) < 0);
}

static void test_profile_defaults_invalid_level(void)
{
	struct vbv_decoder_profile_s dp;
	CHECK(ltntstools_vbv_profile_defaults(&dp, VBV_CODEC_H264, 99, 25) < 0);
}

static void test_profile_defaults_null_arg(void)
{
	CHECK(ltntstools_vbv_profile_defaults(NULL, VBV_CODEC_H264, 31, 25) < 0);
}

/* -------- ltntstools_vbv_profile_validate() -------- */

static void test_profile_validate(void)
{
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 0, .framerate = 25 };
	CHECK(ltntstools_vbv_profile_validate(&dp) == 1);

	dp.framerate = 27.5; /* not one of the accepted broadcast framerates */
	CHECK(ltntstools_vbv_profile_validate(&dp) == 0);
}

/* -------- ltntstools_vbv_event_name() -------- */

static void test_event_name_all_values(void)
{
	CHECK(strcmp(ltntstools_vbv_event_name(EVENT_VBV_UNDEFINED), "EVENT_VBV_UNDEFINED") == 0);
	CHECK(strcmp(ltntstools_vbv_event_name(EVENT_VBV_UNDERFLOW), "EVENT_VBV_UNDERFLOW") == 0);
	CHECK(strcmp(ltntstools_vbv_event_name(EVENT_VBV_OVERFLOW), "EVENT_VBV_OVERFLOW") == 0);
	CHECK(strcmp(ltntstools_vbv_event_name(EVENT_VBV_FULLNESS_PCT_LT_10PCT), "EVENT_VBV_FULLNESS_PCT_LT_10PCT") == 0);
	CHECK(strcmp(ltntstools_vbv_event_name(EVENT_VBV_FULLNESS_PCT_GT_90PCT), "EVENT_VBV_FULLNESS_PCT_GT_90PCT") == 0);
	CHECK(strcmp(ltntstools_vbv_event_name(EVENT_VBV_BPS), "EVENT_VBV_BPS") == 0);
	CHECK(strcmp(ltntstools_vbv_event_name(EVENT_VBV_OOO_DTS), "EVENT_VBV_OOO_DTS") == 0);

	/* Guaranteed to always return a string, even for an out-of-range value. */
	CHECK(ltntstools_vbv_event_name((enum ltntstools_vbv_event_e)9999) != NULL);
	CHECK(strcmp(ltntstools_vbv_event_name((enum ltntstools_vbv_event_e)9999), "EVENT_VBV_UNKNOWN") == 0);
}

/* -------- framerateToNs()/framerateToUs()/framerateToTicks() -------- */

static void test_framerate_conversions(void)
{
	CHECK(framerateToNs(25) == 40000000);
	CHECK(framerateToUs(25) == 40000);
	CHECK(framerateToTicks(25) == 1080000);

	CHECK(framerateToNs(29.97) == 33366700);
	CHECK(framerateToUs(29.97) == 33366);
	CHECK(framerateToTicks(29.97) == 900900);

	CHECK(framerateToNs(60) == 16666666);
	CHECK(framerateToUs(60) == 16666);
	CHECK(framerateToTicks(60) == 450000);
}

/* -------- ltntstools_vbv_alloc() / ltntstools_vbv_free() lifecycle -------- */

static void noop_cb(void *userContext, enum ltntstools_vbv_event_e e)
{
	(void)userContext; (void)e;
}

static void test_alloc_rejects_null_profile(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_vbv_alloc(&hdl, 0x100, noop_cb, NULL, NULL) < 0);
}

static void test_alloc_free_basic(void)
{
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 4000, .framerate = 25 };
	void *hdl = NULL;
	CHECK(ltntstools_vbv_alloc(&hdl, 0x100, noop_cb, NULL, &dp) == 0);
	CHECK(hdl != NULL);
	ltntstools_vbv_free(hdl);
}

static void test_alloc_default_buffer_size_applied(void)
{
	/* vbv_buffer_size == 0 asks the stack to assume its internal default;
	 * confirm via fullness math after writing one known-size packet. */
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 0, .framerate = 25 };
	void *hdl = NULL;
	CHECK(ltntstools_vbv_alloc(&hdl, 0x100, noop_cb, NULL, &dp) == 0);

	struct ltn_pes_packet_s pkt;
	makePkt(&pkt, 90000, 8192);
	CHECK(ltntstools_vbv_write(hdl, &pkt) == 0);

	double pct = -1;
	CHECK(ltntstools_vbv_get_fullness(hdl, &pct) == 0);
	/* default is 800*1024 bytes */
	double expected = ((double)8192 / (double)(800 * 1024)) * 100.0;
	CHECK(pct == expected);

	ltntstools_vbv_free(hdl);
}

static void test_verbose_null_safe(void)
{
	CHECK(ltntstools_vbv_verbose(NULL, 1) < 0);
}

static void test_verbose_sets_level(void)
{
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 4000, .framerate = 25 };
	void *hdl = NULL;
	ltntstools_vbv_alloc(&hdl, 0x100, noop_cb, NULL, &dp);

	CHECK(ltntstools_vbv_verbose(hdl, 1) == 0);

	ltntstools_vbv_free(hdl);
}

/* -------- ltntstools_vbv_write() / ltntstools_vbv_get_fullness() -------- */

static void test_write_null_args(void)
{
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 4000, .framerate = 25 };
	void *hdl = NULL;
	ltntstools_vbv_alloc(&hdl, 0x100, noop_cb, NULL, &dp);

	struct ltn_pes_packet_s pkt;
	makePkt(&pkt, 90000, 100);

	CHECK(ltntstools_vbv_write(NULL, &pkt) < 0);
	CHECK(ltntstools_vbv_write(hdl, NULL) < 0);

	ltntstools_vbv_free(hdl);
}

static void test_get_fullness_null_hdl(void)
{
	double pct = -1;
	CHECK(ltntstools_vbv_get_fullness(NULL, &pct) < 0);
}

static void test_write_accumulates_fullness(void)
{
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 1000, .framerate = 25 };
	void *hdl = NULL;
	ltntstools_vbv_alloc(&hdl, 0x100, noop_cb, NULL, &dp);

	/* Same DTS on every packet: dts_hwm - dts_lwm stays 0, so the
	 * background drain thread never becomes eligible to run, keeping
	 * usedBytes deterministic for this test. */
	struct ltn_pes_packet_s pkt;
	makePkt(&pkt, 90000, 300);
	CHECK(ltntstools_vbv_write(hdl, &pkt) == 0);

	double pct = -1;
	CHECK(ltntstools_vbv_get_fullness(hdl, &pct) == 0);
	CHECK(pct == 30.0);

	makePkt(&pkt, 90000, 200);
	CHECK(ltntstools_vbv_write(hdl, &pkt) == 0);
	CHECK(ltntstools_vbv_get_fullness(hdl, &pct) == 0);
	CHECK(pct == 50.0);

	ltntstools_vbv_free(hdl);
}

struct overflow_capture_s {
	int count;
	enum ltntstools_vbv_event_e lastEvent;
};

static void overflow_cb(void *userContext, enum ltntstools_vbv_event_e e)
{
	struct overflow_capture_s *cap = (struct overflow_capture_s *)userContext;
	cap->count++;
	cap->lastEvent = e;
}

static void test_write_overflow_rejected_and_notified(void)
{
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 1000, .framerate = 25 };
	struct overflow_capture_s cap = { 0 };
	void *hdl = NULL;
	ltntstools_vbv_alloc(&hdl, 0x100, overflow_cb, &cap, &dp);

	struct ltn_pes_packet_s pkt;
	makePkt(&pkt, 90000, 900);
	CHECK(ltntstools_vbv_write(hdl, &pkt) == 0);

	/* 900 + 900 >= 1000: must be rejected as an overflow, and must not
	 * corrupt the bytes already accounted for. */
	makePkt(&pkt, 90000, 900);
	CHECK(ltntstools_vbv_write(hdl, &pkt) < 0);
	CHECK(cap.count == 1);
	CHECK(cap.lastEvent == EVENT_VBV_OVERFLOW);

	double pct = -1;
	CHECK(ltntstools_vbv_get_fullness(hdl, &pct) == 0);
	CHECK(pct == 90.0); /* unchanged by the rejected write */

	ltntstools_vbv_free(hdl);
}

static void test_write_clock_reset_clears_buffer(void)
{
	struct vbv_decoder_profile_s dp = { .vbv_buffer_size = 1000, .framerate = 25 };
	void *hdl = NULL;
	ltntstools_vbv_alloc(&hdl, 0x100, noop_cb, NULL, &dp);

	struct ltn_pes_packet_s pkt;
	makePkt(&pkt, 200000, 300);
	CHECK(ltntstools_vbv_write(hdl, &pkt) == 0);

	double pct = -1;
	CHECK(ltntstools_vbv_get_fullness(hdl, &pct) == 0);
	CHECK(pct == 30.0);

	/* DTS jumps backwards by >= 1 second (90000 ticks @ 90kHz): the model
	 * is reset (buffer cleared) before this packet is accounted for. */
	makePkt(&pkt, 1000, 100);
	CHECK(ltntstools_vbv_write(hdl, &pkt) == 0);
	CHECK(ltntstools_vbv_get_fullness(hdl, &pct) == 0);
	CHECK(pct == 10.0); /* only the post-reset packet counts */

	ltntstools_vbv_free(hdl);
}

int main(void)
{
	test_bitrate_lookup_known_values();
	test_bitrate_lookup_unknown_returns_negative();

	test_profile_defaults_valid();
	test_profile_defaults_invalid_framerate();
	test_profile_defaults_invalid_level();
	test_profile_defaults_null_arg();

	test_profile_validate();

	test_event_name_all_values();

	test_framerate_conversions();

	test_alloc_rejects_null_profile();
	test_alloc_free_basic();
	test_alloc_default_buffer_size_applied();
	test_verbose_null_safe();
	test_verbose_sets_level();

	test_write_null_args();
	test_get_fullness_null_hdl();
	test_write_accumulates_fullness();
	test_write_overflow_rejected_and_notified();
	test_write_clock_reset_clears_buffer();

	if (g_failures == 0) {
		printf("PASS: all vbv tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d vbv test(s) failed\n", g_failures);
	return 1;
}
