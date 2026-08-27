/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/smoother-rtp.c / src/libltntstools/smoother-rtp.h.
 * Builds against ../src/smoother-rtp.c plus ../src/rtp-analyzer.c
 * (rtp_frame_queryPositions(), used to locate RTP-wrapped TS groups in the
 * write() buffer) and ../src/utils.c (ltnpthread_setname_np). Links
 * -lpthread. ltn_histogram_*() and ltntstools_pts_diff() are header-only
 * inline functions, no separate .o needed.
 *
 * This module is smoother-pcr.c's RTP-framed sibling: same background
 * thread pacing design, same real-wall-clock-time-based delivery. These
 * are necessarily timing-based integration tests for the same reason
 * test_smoother_pcr.c's are: real, valid RTP+MPEG-TS groups are written
 * in, then the callback's arrival is polled for with generous timeouts.
 *
 * Framing: rtp_frame_queryPositions() (src/rtp-analyzer.c) looks for a
 * 12-byte RTP header (version=2, payload type=33/MP2T, no extension, no
 * CSRC, matching SSRC) immediately followed by a 0x47 TS sync byte, with a
 * second 0x47 sync byte 188 bytes later. smoother_rtp_write() itself
 * sanity-checks (via a printf, not an assertion) that each processed group
 * is exactly 12+7*188=1328 bytes, so these tests always build groups of
 * exactly that: one 12-byte RTP header + 7 TS packets. It also requires at
 * least 2 detected headers per write() call before anything is queued (the
 * last detected header's length is always unknowable/0 and never
 * processed) -- so N groups in one write() call yields exactly N-1 queued
 * items, mirroring test_smoother_pcr.c's PCR-pair semantics.
 *
 * A REAL bug -- the same use-after-free class already found and fixed in
 * smoother-pcr.c and probes.c -- was found and FIXED in
 * src/smoother-rtp.c's smoother_rtp_alloc(): ctx->threadRunning was set by
 * the background thread itself after it started running, not by the
 * caller at spawn time, so smoother_rtp_free() called quickly enough after
 * alloc() could skip its "wait for the thread to terminate" logic entirely
 * and let the thread dereference an already-freed context. Fixed the same
 * way: set threadRunning = 1 synchronously before pthread_create().
 * test_alloc_free_basic() below stress-loops alloc()+free() with no delay,
 * exactly the pattern that used to race; the fix was additionally verified
 * with repeated AddressSanitizer runs (not part of this file's normal
 * build).
 *
 * Note: unlike smoother_pcr_alloc(), smoother_rtp_alloc() has NO parameter
 * validation at all (any latencyMS is accepted, undersized itemLengthBytes
 * is silently clamped up to the 12+7*188 minimum rather than rejected) --
 * there's no invalid-args test here because there's no rejection path to
 * test. Also, neither smoother_rtp_get_size() nor smoother_rtp_reset() nor
 * smoother_rtp_write() guard against a NULL handle (they dereference it
 * immediately), so -- as with the equivalent smoother-pcr.c functions --
 * this file doesn't test that pattern either.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include "libltntstools/smoother-rtp.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

#define RTP_HDR_LEN 12
#define GROUP_LEN (RTP_HDR_LEN + 7 * 188) /* 1328 */

static const uint8_t g_ssrc[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

/* -------- RTP + TS group construction -------- */

static void build_rtp_group(uint8_t *dst, uint16_t seq, uint32_t ts90k)
{
	dst[0] = 0x80; /* version=2, p=0, x=0, cc=0 */
	dst[1] = 33;   /* m=0, pt=33 (MP2T) */
	dst[2] = (seq >> 8) & 0xff;
	dst[3] = seq & 0xff;
	dst[4] = (ts90k >> 24) & 0xff;
	dst[5] = (ts90k >> 16) & 0xff;
	dst[6] = (ts90k >> 8) & 0xff;
	dst[7] = ts90k & 0xff;
	memcpy(dst + 8, g_ssrc, 4);

	for (int p = 0; p < 7; p++) {
		uint8_t *pkt = dst + RTP_HDR_LEN + p * 188;
		memset(pkt, 0xFF, 188);
		pkt[0] = 0x47;
		pkt[1] = 0x00;
		pkt[2] = 0x00;
		pkt[3] = 0x10; /* pid 0, payload only, cc=0 */
	}
}

/* -------- callback capture (invoked from the smoother's own thread) -------- */

#define MAX_CAPTURES 8
struct captured_s
{
	int byteCount;
	unsigned char buf[GROUP_LEN];
};

static pthread_mutex_t g_captureMutex = PTHREAD_MUTEX_INITIALIZER;
static int g_captureCount;
static struct captured_s g_captures[MAX_CAPTURES];

static void reset_capture(void)
{
	pthread_mutex_lock(&g_captureMutex);
	g_captureCount = 0;
	pthread_mutex_unlock(&g_captureMutex);
}

static int get_capture_count(void)
{
	int n;
	pthread_mutex_lock(&g_captureMutex);
	n = g_captureCount;
	pthread_mutex_unlock(&g_captureMutex);
	return n;
}

static int capture_cb(void *userContext, const unsigned char *buf, int byteCount)
{
	pthread_mutex_lock(&g_captureMutex);
	if (g_captureCount < MAX_CAPTURES) {
		struct captured_s *c = &g_captures[g_captureCount];
		c->byteCount = byteCount;
		memcpy(c->buf, buf, byteCount < GROUP_LEN ? byteCount : GROUP_LEN);
		g_captureCount++;
	}
	pthread_mutex_unlock(&g_captureMutex);
	return 0;
}

static int wait_for_captures(int n, int timeoutMs)
{
	struct timeval start, now;
	gettimeofday(&start, NULL);
	while (1) {
		if (get_capture_count() >= n)
			return 1;
		gettimeofday(&now, NULL);
		int64_t elapsedMs = (now.tv_sec - start.tv_sec) * 1000LL + (now.tv_usec - start.tv_usec) / 1000LL;
		if (elapsedMs >= timeoutMs)
			return 0;
		usleep(2 * 1000);
	}
}

static int wait_for_size(void *hdl, int64_t expected, int timeoutMs)
{
	struct timeval start, now;
	gettimeofday(&start, NULL);
	while (1) {
		if (smoother_rtp_get_size(hdl) == expected)
			return 1;
		gettimeofday(&now, NULL);
		int64_t elapsedMs = (now.tv_sec - start.tv_sec) * 1000LL + (now.tv_usec - start.tv_usec) / 1000LL;
		if (elapsedMs >= timeoutMs)
			return 0;
		usleep(2 * 1000);
	}
}

static void *alloc_smoother(int itemsPerSecond, int latencyMS)
{
	void *hdl = NULL;
	int ret = smoother_rtp_alloc(&hdl, NULL, capture_cb, itemsPerSecond, GROUP_LEN, latencyMS);
	if (ret != 0)
		return NULL;
	return hdl;
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	/* Stress the alloc()-immediately-followed-by-free() pattern that used
	 * to race (see file header). */
	for (int i = 0; i < 20; i++) {
		void *hdl = alloc_smoother(16, 50);
		CHECK(hdl != NULL);
		smoother_rtp_free(hdl);
	}
}

static void test_undersized_item_length_is_clamped_not_rejected(void)
{
	void *hdl = NULL;
	CHECK(smoother_rtp_alloc(&hdl, NULL, capture_cb, 16, 1 /* far below the 1328 minimum */, 50) == 0);
	CHECK(hdl != NULL);
	smoother_rtp_free(hdl);
}

static void test_get_size_zero_after_alloc(void)
{
	void *hdl = alloc_smoother(16, 50);
	CHECK(hdl != NULL);

	CHECK(smoother_rtp_get_size(hdl) == 0);

	smoother_rtp_free(hdl);
}

/* -------- single group: delivery + content correctness -------- */

static void test_two_groups_deliver_one_matching_item(void)
{
	void *hdl = alloc_smoother(16, 50 /* minimum-ish latency, keep test fast */);
	CHECK(hdl != NULL);
	reset_capture();

	uint8_t groups[2][GROUP_LEN];
	build_rtp_group(groups[0], 1000, 27000000 /* arbitrary base ts, 90KHz units */);
	build_rtp_group(groups[1], 1001, 27000000 + 2700 /* negligible vs latency */);

	struct timeval ts;
	gettimeofday(&ts, NULL);
	CHECK(smoother_rtp_write(hdl, &groups[0][0], sizeof(groups), &ts) == 0);

	/* Only the first group is queued -- the second's length is
	 * unknowable (no third header found yet) so it stays buffered. */
	CHECK(smoother_rtp_get_size(hdl) == GROUP_LEN);

	CHECK(wait_for_captures(1, 2000));
	CHECK(get_capture_count() == 1);

	pthread_mutex_lock(&g_captureMutex);
	struct captured_s c = g_captures[0];
	pthread_mutex_unlock(&g_captureMutex);

	CHECK(c.byteCount == GROUP_LEN);
	CHECK(memcmp(c.buf, groups[0], GROUP_LEN) == 0);

	CHECK(wait_for_size(hdl, 0, 500));

	smoother_rtp_free(hdl);
}

/* -------- multiple groups: pacing spreads delivery over time -------- */

static void test_three_groups_are_paced_not_delivered_together(void)
{
	void *hdl = alloc_smoother(16, 50);
	CHECK(hdl != NULL);
	reset_capture();

	uint32_t ts0 = 27000000;
	uint32_t tsDeltaTicks = 90000 * 140 / 1000; /* 140ms of 90KHz ticks -> 12600 */

	uint8_t groups[3][GROUP_LEN];
	build_rtp_group(groups[0], 2000, ts0);
	build_rtp_group(groups[1], 2001, ts0 + tsDeltaTicks);
	build_rtp_group(groups[2], 2002, ts0 + 2 * tsDeltaTicks);

	struct timeval ts;
	gettimeofday(&ts, NULL);
	CHECK(smoother_rtp_write(hdl, &groups[0][0], sizeof(groups), &ts) == 0);

	/* Item 1 (ts=ts0) is due at ~latency (50ms): ticks-from-tsFirst is 0
	 * for the very first item. Item 2 (ts=ts0+delta) is due at
	 * ~140ms+latency (190ms). Checking partway through confirms they're
	 * genuinely paced apart. */
	CHECK(wait_for_captures(1, 2000));
	CHECK(!wait_for_captures(2, 60)); /* at ~110ms total: item 2 not due yet */
	CHECK(get_capture_count() == 1);

	CHECK(wait_for_captures(2, 2000));
	CHECK(get_capture_count() == 2);

	pthread_mutex_lock(&g_captureMutex);
	struct captured_s first = g_captures[0];
	struct captured_s second = g_captures[1];
	pthread_mutex_unlock(&g_captureMutex);

	CHECK(memcmp(first.buf, groups[0], GROUP_LEN) == 0);
	CHECK(memcmp(second.buf, groups[1], GROUP_LEN) == 0);

	smoother_rtp_free(hdl);
}

/* -------- reset() discards queued-but-undelivered content -------- */

static void test_reset_discards_queued_content_before_delivery(void)
{
	void *hdl = alloc_smoother(16, 50);
	CHECK(hdl != NULL);
	reset_capture();

	uint8_t groups[2][GROUP_LEN];
	build_rtp_group(groups[0], 3000, 27000000);
	build_rtp_group(groups[1], 3001, 27000000 + 2700);

	struct timeval ts;
	gettimeofday(&ts, NULL);
	smoother_rtp_write(hdl, &groups[0][0], sizeof(groups), &ts);

	CHECK(smoother_rtp_get_size(hdl) == GROUP_LEN);

	smoother_rtp_reset(hdl);

	CHECK(smoother_rtp_get_size(hdl) == 0);

	/* Confirm it really never arrives, not just "not yet". */
	CHECK(!wait_for_captures(1, 300));
	CHECK(get_capture_count() == 0);

	smoother_rtp_free(hdl);
}

int main(void)
{
	test_alloc_free_basic();
	test_undersized_item_length_is_clamped_not_rejected();
	test_get_size_zero_after_alloc();

	test_two_groups_deliver_one_matching_item();
	test_three_groups_are_paced_not_delivered_together();
	test_reset_discards_queued_content_before_delivery();

	if (g_failures == 0) {
		printf("PASS: all smoother-rtp tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d smoother-rtp test(s) failed\n", g_failures);
	return 1;
}
