/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/probes.c / src/libltntstools/probes.h.
 * Builds against ../src/probes.c plus ../src/sei-timestamp.c (LTN encoder
 * UUID/timestamp parsing) and ../src/utils.c (ltnpthread_setname_np).
 * Links -lpthread. ltn_histogram_*() and ltn_timeval_subtract_ms() are
 * header-only inline functions, no separate .o needed. Includes the
 * library-private src/sei-timestamp.h directly (as probes.c itself does)
 * to build synthetic LTN-encoder SEI timestamp payloads with
 * ltn_uuid_sei_timestamp/sei_timestamp_*().
 *
 * TWO REAL, CHAINED bugs were found while writing these tests and have
 * been FIXED:
 *
 * 1. src/sei-timestamp.c: sei_timestamp_value_timeval_query() read two
 *    fields via sei_timestamp_field_get() into local `secs`/`usecs`
 *    variables and ignored both return codes, then unconditionally copied
 *    those (possibly still-uninitialized, on failure) locals into the
 *    caller's `*t` and returned 0 (success) regardless. Its counterpart
 *    sei_timestamp_value_timeval_set() had the same ignore-the-return-code
 *    pattern (silent partial write reported as success). Fixed by
 *    propagating field_get()/field_set() failure as -1 without touching
 *    the output/buffer.
 *
 * 2. src/probes.c: ltntstools_probe_ltnencoder_sei_timestamp_query()
 *    treated "the LTN UUID was found somewhere in the buffer" as
 *    sufficient for success, without checking whether the subsequent
 *    timestamp field extraction actually succeeded. Combined with bug #1,
 *    a UUID found near the tail of a buffer -- with too few trailing bytes
 *    left for fields 2/3 (frame-entry sec/usec) -- silently reported
 *    success (return 0) with ctx->latencyMs computed from uninitialized
 *    stack memory. Confirmed via repro: a 100-byte buffer with the UUID
 *    placed at offset 80 (only 20 trailing bytes -- fields 2/3 need 34)
 *    returned 0 with garbage latency before the fix; now correctly
 *    returns -1 and leaves latency at its -1 default. Fixed by making
 *    _ltnencoder_sei_timestamp_query() return the underlying query's
 *    result and propagating it.
 *
 * A THIRD real bug -- the same use-after-free class already found and
 * fixed in smoother-pcr.c -- was found and fixed in
 * ltntstools_probe_scheduler_alloc(): ctx->threadRunning was set by the
 * background thread itself after it started running, not by the caller at
 * spawn time, so ltntstools_probe_scheduler_free() called quickly enough
 * after alloc() could skip its "wait for the thread to terminate" logic
 * entirely and let the thread dereference an already-freed context. Fixed
 * the same way: set threadRunning = 1 synchronously before pthread_create().
 * test_scheduler_alloc_free_basic() below stress-loops alloc()+free() with
 * no delay between them, which is exactly the pattern that used to race.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>

#include "libltntstools/probes.h"
#include "sei-timestamp.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- ltnencoder probe: lifecycle -------- */

static void test_ltnencoder_alloc_free_basic(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_probe_ltnencoder_alloc(&hdl) == 0);
	CHECK(hdl != NULL);
	ltntstools_probe_ltnencoder_free(hdl);
}

static void test_ltnencoder_get_total_latency_null_hdl_returns_negative(void)
{
	CHECK(ltntstools_probe_ltnencoder_get_total_latency(NULL) == -1);
}

static void test_ltnencoder_get_total_latency_default_is_negative_one(void)
{
	void *hdl = NULL;
	ltntstools_probe_ltnencoder_alloc(&hdl);

	CHECK(ltntstools_probe_ltnencoder_get_total_latency(hdl) == -1);

	ltntstools_probe_ltnencoder_free(hdl);
}

/* -------- ltnencoder probe: SEI timestamp query -------- */

static void test_ltnencoder_query_no_uuid_fails(void)
{
	void *hdl = NULL;
	ltntstools_probe_ltnencoder_alloc(&hdl);

	unsigned char buf[128];
	memset(buf, 0, sizeof(buf));

	CHECK(ltntstools_probe_ltnencoder_sei_timestamp_query(hdl, buf, sizeof(buf)) == -1);
	CHECK(ltntstools_probe_ltnencoder_get_total_latency(hdl) == -1);

	ltntstools_probe_ltnencoder_free(hdl);
}

static void test_ltnencoder_query_success_computes_latency(void)
{
	void *hdl = NULL;
	ltntstools_probe_ltnencoder_alloc(&hdl);

	/* Embed a full LTN-encoder SEI timestamp payload after some junk
	 * prefix bytes, mimicking a NAL/TS buffer where the UUID isn't at
	 * offset 0. Field 2/3 ("frame entry" time, per sei-timestamp.h) set
	 * to 300ms in the past. */
	unsigned char prefix[16];
	memset(prefix, 0xAA, sizeof(prefix));

	unsigned char payload[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	sei_timestamp_init(payload, sizeof(payload));

	struct timeval frameEntry;
	gettimeofday(&frameEntry, NULL);
	if (frameEntry.tv_usec < 300000) {
		frameEntry.tv_sec -= 1;
		frameEntry.tv_usec += 700000;
	} else {
		frameEntry.tv_usec -= 300000;
	}
	sei_timestamp_value_timeval_set(payload, sizeof(payload), 2, &frameEntry);

	unsigned char buf[sizeof(prefix) + sizeof(payload)];
	memcpy(buf, prefix, sizeof(prefix));
	memcpy(buf + sizeof(prefix), payload, sizeof(payload));

	struct timeval before, after;
	gettimeofday(&before, NULL);
	int ret = ltntstools_probe_ltnencoder_sei_timestamp_query(hdl, buf, sizeof(buf));
	gettimeofday(&after, NULL);
	CHECK(ret == 0);

	int64_t latencyMs = ltntstools_probe_ltnencoder_get_total_latency(hdl);
	/* Loosely bracket: at least ~250ms (allowing scheduling slop below
	 * the 300ms target), and no more than the true elapsed wall time
	 * between "before" and the frame-entry timestamp we set (300ms plus
	 * whatever this test itself took to run up to "after"). */
	int64_t maxExpectedMs = 300 + (after.tv_sec - before.tv_sec) * 1000 + (after.tv_usec - before.tv_usec) / 1000 + 50;
	CHECK(latencyMs >= 250);
	CHECK(latencyMs <= maxExpectedMs);

	ltntstools_probe_ltnencoder_free(hdl);
}

/* Regression test for the fixed bug chain: see file header. */
static void test_ltnencoder_query_uuid_too_close_to_end_fails_cleanly(void)
{
	void *hdl = NULL;
	ltntstools_probe_ltnencoder_alloc(&hdl);

	/* Total buffer >= SEI_TIMESTAMP_PAYLOAD_LENGTH (satisfies
	 * ltn_uuid_find()'s own whole-buffer gate), but the UUID is placed
	 * near the tail so only 20 bytes remain after it -- enough for
	 * field 1 (needs 22) to still fail too, well short of what fields
	 * 2/3 need (34). */
	unsigned char buf[100];
	memset(buf, 0, sizeof(buf));
	CHECK(sizeof(buf) >= SEI_TIMESTAMP_PAYLOAD_LENGTH);

	int uuidOffset = 80;
	CHECK(uuidOffset + (int)sizeof(ltn_uuid_sei_timestamp) <= (int)sizeof(buf));
	memcpy(buf + uuidOffset, ltn_uuid_sei_timestamp, sizeof(ltn_uuid_sei_timestamp));

	CHECK(ltntstools_probe_ltnencoder_sei_timestamp_query(hdl, buf, sizeof(buf)) == -1);
	/* Must still report the documented "no timing information detected"
	 * default, not garbage derived from uninitialized memory. */
	CHECK(ltntstools_probe_ltnencoder_get_total_latency(hdl) == -1);

	ltntstools_probe_ltnencoder_free(hdl);
}

/* -------- scheduler probe: lifecycle / threadRunning race regression -------- */

static void test_scheduler_alloc_free_basic(void)
{
	/* Stress the alloc()-immediately-followed-by-free() pattern that used
	 * to race (see file header): repeat several times to raise the odds
	 * of catching a reintroduction even without a sanitizer. */
	for (int i = 0; i < 20; i++) {
		void *hdl = NULL;
		CHECK(ltntstools_probe_scheduler_alloc(&hdl) == 0);
		CHECK(hdl != NULL);
		ltntstools_probe_scheduler_free(hdl);
	}
}

static void test_scheduler_getters_reject_null_hdl(void)
{
	CHECK(ltntstools_probe_scheduler_get_3ms_error_count(NULL) == -1);

	char *buf = NULL;
	CHECK(ltntstools_probe_scheduler_get_histogram_report(NULL, &buf) == -1);
}

static void test_scheduler_reports_plausible_output_after_running(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_probe_scheduler_alloc(&hdl) == 0);

	/* Let the background thread accumulate a handful of 1ms-sleep
	 * samples. The error count itself is real OS scheduler jitter and
	 * not something to assert an exact value for -- just that the API
	 * produces sane, non-error output once it's had a chance to run. */
	usleep(50 * 1000);

	CHECK(ltntstools_probe_scheduler_get_3ms_error_count(hdl) >= 0);

	char *report = NULL;
	CHECK(ltntstools_probe_scheduler_get_histogram_report(hdl, &report) == 0);
	CHECK(report != NULL);
	if (report) {
		CHECK(strlen(report) > 0);
		free(report);
	}

	ltntstools_probe_scheduler_free(hdl);
}

int main(void)
{
	test_ltnencoder_alloc_free_basic();
	test_ltnencoder_get_total_latency_null_hdl_returns_negative();
	test_ltnencoder_get_total_latency_default_is_negative_one();

	test_ltnencoder_query_no_uuid_fails();
	test_ltnencoder_query_success_computes_latency();
	test_ltnencoder_query_uuid_too_close_to_end_fails_cleanly();

	test_scheduler_alloc_free_basic();
	test_scheduler_getters_reject_null_hdl();
	test_scheduler_reports_plausible_output_after_running();

	if (g_failures == 0) {
		printf("PASS: all probes tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d probes test(s) failed\n", g_failures);
	return 1;
}
