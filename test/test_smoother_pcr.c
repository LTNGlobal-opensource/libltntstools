/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/smoother-pcr.c / src/libltntstools/smoother-pcr.h.
 * Builds against ../src/smoother-pcr.c plus ../src/ts.c (ltntstools_queryPCRs,
 * ltntstools_generatePCROnlyPacket, ltntstools_pcr_position_append, pid/scr
 * helpers) and ../src/utils.c (ltnpthread_setname_np). Links -lpthread.
 * ltn_histogram_*() and ltntstools_scr_diff/add() are header-only inline
 * functions (histogram.h, ts.h), no separate .o needed.
 *
 * This module runs a real background thread that paces callback delivery
 * against real wall-clock time (gettimeofday), so these are necessarily
 * timing-based integration tests, not pure unit tests: they write real,
 * valid, PCR-bearing TS packets (built with ltntstools_generatePCROnlyPacket(),
 * already unit-tested indirectly via ts.c's own consumers) and then poll
 * (with generous timeouts) for the callback to fire, asserting delivery
 * happens within a bounded window rather than at an exact instant.
 *
 * TWO REAL bugs were found and FIXED in src/smoother-pcr.c while writing
 * these tests:
 *
 * 1. (Critical) A genuine use-after-free race, 100% reproducible under
 *    AddressSanitizer with nothing more than the simplest possible
 *    smoother_pcr_alloc() immediately followed by smoother_pcr_free() (no
 *    writes, no delay -- see test_alloc_free_basic()). smoother_pcr_free()
 *    only waits for the background thread to exit `if (ctx->threadRunning)`
 *    -- but threadRunning was set by the CHILD thread itself, as the first
 *    thing it does after actually getting scheduled, not by the parent at
 *    pthread_create() time. If free() runs before the new thread has been
 *    scheduled even once, threadRunning still reads its calloc()'d 0,
 *    free()'s "wait for the thread to terminate" block is skipped
 *    entirely, and the thread goes on to dereference ctx (in its
 *    `while (!ctx->threadTerminate)` loop condition, among other places)
 *    after free() has already deallocated it. Confirmed via ASan
 *    (heap-use-after-free at smoother-pcr.c:491) and via plain crashes
 *    under repeated runs without a sanitizer. Fixed by setting
 *    ctx->threadRunning = 1 synchronously in smoother_pcr_alloc(), before
 *    pthread_create(), closing the race window entirely.
 *
 * 2. smoother_pcr_alloc() calloc()'d its context BEFORE validating
 *    pcrPID/latencyMS/itemLengthBytes, and returned -1 on invalid args
 *    without ever freeing that allocation -- every rejected
 *    smoother_pcr_alloc() call (an explicitly documented, expected failure
 *    path) leaked sizeof(struct smoother_pcr_context_s). Fixed by
 *    validating args before the calloc().
 *
 * TWO issues were found but deliberately NOT fixed (out of scope /
 * unconfirmable without fault injection, and this is live real-time
 * broadcast-pacing code where behavior changes carry real risk):
 *  1. itemAlloc() dereferences `item` (item->maxLengthBytes = ...) after a
 *     branch that sets `item = NULL` on item->buf allocation failure --
 *     a NULL-pointer-deref crash, but only reachable under OOM, which can't
 *     be deterministically triggered/confirmed in a normal unit test.
 *  2. smoother_pcr_threadFunc()'s pthread_cond_timedwait() uses a static
 *     `struct timespec abstime = {0, 50*1000}` (interpreted as an absolute
 *     CLOCK_REALTIME deadline of 1970-01-01 00:00:00.00005, i.e. always
 *     already-expired) instead of "now + 50ms", recomputed each iteration.
 *     The wait therefore never actually blocks: whenever the busy queue is
 *     empty, the thread busy-spins at 100% of a core instead of sleeping.
 *     This doesn't produce incorrect output (functionally harmless to the
 *     tests below -- if anything it makes delivery timing tighter/more
 *     predictable), so it's a performance characteristic worth flagging,
 *     not something these tests fix.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

/* ts.h must precede smoother-pcr.h: smoother-pcr.h's callback typedef uses
 * struct ltntstools_pcr_position_s without declaring/including it, so it
 * needs the real definition already in scope for capture_cb's signature
 * below to match smoother_pcr_output_callback. */
#include "libltntstools/ts.h"
#include "libltntstools/smoother-pcr.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

#define ITEM_LENGTH_BYTES (7 * 188)
#define PCR_PID 0x100
#define FILLER_PID 0x101

/* -------- packet construction -------- */

static void build_filler_packet(uint8_t *pkt, uint16_t pid, uint8_t *cc)
{
	memset(pkt, 0xFF, 188);
	pkt[0] = 0x47;
	pkt[1] = (pid >> 8) & 0x1f;
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10 | ((*cc)++ & 0x0f); /* payload only, no adaptation/PCR */
}

/* Builds a PCR interval: one PCR-only packet at `pcr`, then `fillerCount`
 * plain payload packets on FILLER_PID. Total packets = 1 + fillerCount. */
static int build_pcr_interval(uint8_t packets[][188], uint8_t *pcrCc, uint8_t *fillerCc,
	uint64_t pcr, int fillerCount)
{
	int n = 0;
	ltntstools_generatePCROnlyPacket(packets[n++], 188, PCR_PID, pcrCc, pcr);
	for (int i = 0; i < fillerCount; i++) {
		build_filler_packet(packets[n++], FILLER_PID, fillerCc);
	}
	return n;
}

/* -------- callback capture (invoked from the smoother's own thread) -------- */

#define MAX_CAPTURES 8
struct captured_s
{
	int byteCount;
	unsigned char buf[ITEM_LENGTH_BYTES];
	int arrayLength;
	struct ltntstools_pcr_position_s pcrs[7];
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

static int capture_cb(void *userContext, unsigned char *buf, int byteCount,
	struct ltntstools_pcr_position_s *array, int arrayLength)
{
	pthread_mutex_lock(&g_captureMutex);
	if (g_captureCount < MAX_CAPTURES) {
		struct captured_s *c = &g_captures[g_captureCount];
		c->byteCount = byteCount;
		memcpy(c->buf, buf, byteCount < ITEM_LENGTH_BYTES ? byteCount : ITEM_LENGTH_BYTES);
		c->arrayLength = arrayLength < 7 ? arrayLength : 7;
		memcpy(c->pcrs, array, c->arrayLength * sizeof(*array));
		g_captureCount++;
	}
	pthread_mutex_unlock(&g_captureMutex);
	return 0;
}

/* Polls (rather than sleeping a fixed amount) up to timeoutMs for at least
 * `n` captures to have arrived. Returns 1 if reached, 0 on timeout -- avoids
 * both flakiness under CI load and needlessly slow tests. */
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

/* _queueProcess() invokes the callback BEFORE it decrements
 * ctx->totalUserBytes for that item, so a test synchronizing on "the
 * callback fired" (via wait_for_captures()) can race a get_size() check
 * that still reflects the item as in-flight for a few more instructions.
 * Poll instead of asserting immediately after a capture. */
static int wait_for_size(void *hdl, int64_t expected, int timeoutMs)
{
	struct timeval start, now;
	gettimeofday(&start, NULL);
	while (1) {
		if (smoother_pcr_get_size(hdl) == expected)
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
	int ret = smoother_pcr_alloc(&hdl, NULL, capture_cb, itemsPerSecond, ITEM_LENGTH_BYTES, PCR_PID, latencyMS);
	if (ret != 0)
		return NULL;
	return hdl;
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	void *hdl = alloc_smoother(16, 50);
	CHECK(hdl != NULL);
	smoother_pcr_free(hdl);
}

static void test_alloc_rejects_invalid_params(void)
{
	void *hdl;

	hdl = (void *)0x1;
	CHECK(smoother_pcr_alloc(&hdl, NULL, capture_cb, 16, ITEM_LENGTH_BYTES, 16 /* too low, must be > 16 */, 50) == -1);
	CHECK(hdl == (void *)0x1); /* untouched on failure */

	hdl = (void *)0x1;
	CHECK(smoother_pcr_alloc(&hdl, NULL, capture_cb, 16, ITEM_LENGTH_BYTES, 0x1fff /* too high */, 50) == -1);

	hdl = (void *)0x1;
	CHECK(smoother_pcr_alloc(&hdl, NULL, capture_cb, 16, ITEM_LENGTH_BYTES, PCR_PID, 49 /* < 50 min */) == -1);

	hdl = (void *)0x1;
	CHECK(smoother_pcr_alloc(&hdl, NULL, capture_cb, 16, ITEM_LENGTH_BYTES - 1 /* must be exactly 7*188 */, PCR_PID, 50) == -1);
}

static void test_setters_and_getters_reject_null_hdl(void)
{
	CHECK(smoother_pcr_set_verbose(NULL, 1) == -1);
	CHECK(smoother_pcr_set_blocking_writes(NULL, 1) == -1);
	struct smoother_pcr_statistics s;
	CHECK(smoother_pcr_get_statistics(NULL, &s) == -1);
}

static void test_setters_return_success(void)
{
	void *hdl = alloc_smoother(16, 50);
	CHECK(hdl != NULL);

	CHECK(smoother_pcr_set_verbose(hdl, 1) == 0);
	CHECK(smoother_pcr_set_verbose(hdl, 0) == 0);
	CHECK(smoother_pcr_set_blocking_writes(hdl, 0) == 0);

	smoother_pcr_free(hdl);
}

/* -------- statistics / size right after alloc -------- */

static void test_statistics_and_size_immediately_after_alloc(void)
{
	void *hdl = alloc_smoother(32, 50);
	CHECK(hdl != NULL);

	struct smoother_pcr_statistics s;
	memset(&s, 0xff, sizeof(s));
	CHECK(smoother_pcr_get_statistics(hdl, &s) == 0);
	CHECK(s.totalItems == 32);
	CHECK(s.qFreeCount == 32);
	CHECK(s.qBusyCount == 0);
	CHECK(s.totalUserBytes == 0);
	CHECK(s.totalItemGrowth == 0);

	CHECK(smoother_pcr_get_size(hdl) == 0);

	smoother_pcr_free(hdl);
}

/* -------- single PCR interval: delivery + content correctness -------- */

static void test_single_interval_delivers_matching_content(void)
{
	void *hdl = alloc_smoother(16, 50 /* minimum latency, keep test fast */);
	CHECK(hdl != NULL);
	reset_capture();

	uint8_t packets[8][188];
	uint8_t pcrCc = 0, fillerCc = 0;
	uint64_t pcr0 = 27000000ULL * 10; /* arbitrary base, 10s worth of ticks */
	uint64_t perPacketTicks = 2700;   /* 100us/packet -- negligible vs latency */
	int n = build_pcr_interval(packets, &pcrCc, &fillerCc, pcr0, 6);
	CHECK(n == 7);
	/* A second PCR is required to close the interval (queryPCRs needs 2
	 * matches for pcrPID before smoother_pcr_write() will process
	 * anything at all). */
	ltntstools_generatePCROnlyPacket(packets[n++], 188, PCR_PID, &pcrCc, pcr0 + 7 * perPacketTicks);
	CHECK(n == 8);

	struct timeval ts;
	gettimeofday(&ts, NULL);
	CHECK(smoother_pcr_write(hdl, &packets[0][0], n * 188, &ts) == 0);

	/* Exactly one 7*188-byte item should have been queued (the 8th packet,
	 * the closing PCR, isn't consumed -- it stays buffered as the start of
	 * the next interval). */
	CHECK(smoother_pcr_get_size(hdl) == ITEM_LENGTH_BYTES);

	CHECK(wait_for_captures(1, 2000));
	CHECK(get_capture_count() == 1);

	pthread_mutex_lock(&g_captureMutex);
	struct captured_s c = g_captures[0];
	pthread_mutex_unlock(&g_captureMutex);

	CHECK(c.byteCount == ITEM_LENGTH_BYTES);
	CHECK(memcmp(c.buf, packets, ITEM_LENGTH_BYTES) == 0);
	CHECK(c.arrayLength == 7);
	if (c.arrayLength == 7) {
		CHECK(c.pcrs[0].pid == PCR_PID);
		CHECK((uint64_t)c.pcrs[0].pcr == pcr0);
		for (int i = 1; i < 7; i++) {
			CHECK(c.pcrs[i].pid == FILLER_PID);
			CHECK((uint64_t)c.pcrs[i].pcr == pcr0 + (uint64_t)i * perPacketTicks);
		}
	}

	CHECK(wait_for_size(hdl, 0, 500));

	smoother_pcr_free(hdl);
}

/* -------- two PCR intervals: pacing spreads delivery over time -------- */

static void test_two_intervals_are_paced_not_delivered_together(void)
{
	void *hdl = alloc_smoother(16, 50);
	CHECK(hdl != NULL);
	reset_capture();

	uint8_t packets[16][188];
	uint8_t pcrCc = 0, fillerCc = 0;
	uint64_t pcr0 = 27000000ULL * 10;
	uint64_t perPacketTicks = 27000 * 20; /* 20ms/packet -> 140ms per 7-packet interval */

	int n = 0;
	n += build_pcr_interval(packets + n, &pcrCc, &fillerCc, pcr0, 6);                          /* interval 1 start */
	uint64_t pcr1 = pcr0 + 7 * perPacketTicks;                                                 /* +140ms */
	n += build_pcr_interval(packets + n, &pcrCc, &fillerCc, pcr1, 6);                          /* interval 2 start / interval 1 end */
	uint64_t pcr2 = pcr1 + 7 * perPacketTicks;                                                 /* +140ms */
	ltntstools_generatePCROnlyPacket(packets[n++], 188, PCR_PID, &pcrCc, pcr2);                /* interval 2 end */
	CHECK(n == 15);

	struct timeval ts;
	gettimeofday(&ts, NULL);
	CHECK(smoother_pcr_write(hdl, &packets[0][0], n * 188, &ts) == 0);

	/* Item 1 (pcrValue=pcr0) is due at ~latency (50ms): ticks-from-pcrFirst
	 * is 0 for the very first item. Item 2 (pcrValue=pcr1) is due at
	 * ~140ms+latency (190ms), since it's scheduled 140ms of PCR-ticks
	 * after pcrFirst. Checking partway through that window (well past
	 * item 1's due time, well before item 2's) confirms they're genuinely
	 * paced apart, not both dumped out together. */
	CHECK(wait_for_captures(1, 2000));
	CHECK(!wait_for_captures(2, 60)); /* at ~110ms total: item 2 not due yet */
	CHECK(get_capture_count() == 1);

	CHECK(wait_for_captures(2, 2000));
	CHECK(get_capture_count() == 2);

	pthread_mutex_lock(&g_captureMutex);
	struct captured_s first = g_captures[0];
	struct captured_s second = g_captures[1];
	pthread_mutex_unlock(&g_captureMutex);

	CHECK(memcmp(first.buf, packets[0], ITEM_LENGTH_BYTES) == 0);
	CHECK(memcmp(second.buf, packets[7], ITEM_LENGTH_BYTES) == 0);
	CHECK((uint64_t)first.pcrs[0].pcr == pcr0);
	CHECK((uint64_t)second.pcrs[0].pcr == pcr1);

	smoother_pcr_free(hdl);
}

/* -------- reset() discards queued-but-undelivered content -------- */

static void test_reset_discards_queued_content_before_delivery(void)
{
	void *hdl = alloc_smoother(16, 50);
	CHECK(hdl != NULL);
	reset_capture();

	uint8_t packets[8][188];
	uint8_t pcrCc = 0, fillerCc = 0;
	uint64_t pcr0 = 27000000ULL * 10;
	/* Large gap so this item is due well in the future -- reset() should
	 * discard it long before that. */
	uint64_t perPacketTicks = 27000 * 100; /* 100ms/packet -> 700ms interval */

	int n = build_pcr_interval(packets, &pcrCc, &fillerCc, pcr0, 6);
	ltntstools_generatePCROnlyPacket(packets[n++], 188, PCR_PID, &pcrCc, pcr0 + 7 * perPacketTicks);

	struct timeval ts;
	gettimeofday(&ts, NULL);
	smoother_pcr_write(hdl, &packets[0][0], n * 188, &ts);

	CHECK(smoother_pcr_get_size(hdl) == ITEM_LENGTH_BYTES);

	smoother_pcr_reset(hdl);

	CHECK(smoother_pcr_get_size(hdl) == 0);
	struct smoother_pcr_statistics s;
	smoother_pcr_get_statistics(hdl, &s);
	CHECK(s.qBusyCount == 0);

	/* Confirm it really never arrives, not just "not yet". */
	CHECK(!wait_for_captures(1, 300));
	CHECK(get_capture_count() == 0);

	smoother_pcr_free(hdl);
}

int main(void)
{
	test_alloc_free_basic();
	test_alloc_rejects_invalid_params();
	test_setters_and_getters_reject_null_hdl();
	test_setters_return_success();

	test_statistics_and_size_immediately_after_alloc();

	test_single_interval_delivers_matching_content();
	test_two_intervals_are_paced_not_delivered_together();
	test_reset_discards_queued_content_before_delivery();

	if (g_failures == 0) {
		printf("PASS: all smoother-pcr tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d smoother-pcr test(s) failed\n", g_failures);
	return 1;
}
