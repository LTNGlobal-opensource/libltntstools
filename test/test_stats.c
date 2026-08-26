/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/stats.c / src/libltntstools/stats.h.
 * Builds against ../src/stats.c plus its real (non-mocked) dependencies
 * ../src/clocks.c, ../src/ts.c and ../src/history-metric.c (histogram.h and
 * xorg-list.h are header-only). Requires -lpthread.
 *
 * SCOPE NOTE: stream/pid `pps`/`mbps`/`bps` (and the A/324 Bps/mbps
 * equivalents) are published once per real wall-clock second
 * (`now != pps_last_update`), and the IAT high-water-mark-per-N-seconds
 * fields only update every 5 real seconds. Asserting specific throughput
 * values would require sleeping across real time boundaries, so this file
 * does not attempt it -- see test_first_update_call_pps_quirk() for the one
 * *deterministic* consequence of that windowing that's exercised instead.
 *
 * All non-trivial expected numeric values below (CC-error thresholds, the
 * 100-packet PCR warm-up boundary, the bitrate calculator's bps/ticks/stc
 * outputs, CTP sequence-number behaviour) were first confirmed against the
 * real implementation with standalone repro programs before being hard
 * coded here as expected values.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "libltntstools/ltntstools.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

static void set_header(uint8_t *pkt, uint16_t pid, int tei, int pusi, uint8_t afc, uint8_t sc, uint8_t cc)
{
	memset(pkt, 0xff, 188);
	pkt[0] = 0x47;
	pkt[1] = (tei ? 0x80 : 0) | (pusi ? 0x40 : 0) | ((pid >> 8) & 0x1f);
	pkt[2] = pid & 0xff;
	pkt[3] = ((sc & 0x3) << 6) | ((afc & 0x3) << 4) | (cc & 0x0f);
}

/* -------- pure helper functions -------- */

static void test_isCCInError_adaptation_only_or_reserved(void)
{
	uint8_t pkt[188];

	set_header(pkt, 0x100, 0, 0, 2 /* adaptation only */, 0, 5);
	CHECK(ltntstools_isCCInError(pkt, 5) == 0);  /* unchanged CC: ok */
	CHECK(ltntstools_isCCInError(pkt, 4) == 0);  /* CC advanced by 1: also accepted */
	CHECK(ltntstools_isCCInError(pkt, 3) == 1);  /* CC jumped: error */
}

static void test_isCCInError_payload_bearing(void)
{
	uint8_t pkt[188];

	set_header(pkt, 0x100, 0, 0, 1 /* payload only */, 0, 5);
	CHECK(ltntstools_isCCInError(pkt, 5) == 1);  /* stale/duplicate CC: error */
	CHECK(ltntstools_isCCInError(pkt, 4) == 0);  /* correctly advanced by 1: ok */
	CHECK(ltntstools_isCCInError(pkt, 3) == 1);  /* skipped a value: error */
}

static void test_isCCInError_wraparound(void)
{
	uint8_t pkt[188];
	set_header(pkt, 0x100, 0, 0, 1, 0, 0 /* cc wraps 15 -> 0 */);
	CHECK(ltntstools_isCCInError(pkt, 15) == 0);
}

static void test_isPayloadPUSIInError(void)
{
	uint8_t pkt[188];

	set_header(pkt, 0x100, 0, 1, 2 /* adaptation only */, 0, 0);
	CHECK(ltntstools_isPayloadPUSIInError(pkt) == 1);

	set_header(pkt, 0x100, 0, 0, 2, 0, 0);
	CHECK(ltntstools_isPayloadPUSIInError(pkt) == 0);

	set_header(pkt, 0x100, 0, 1, 1 /* payload only */, 0, 0);
	CHECK(ltntstools_isPayloadPUSIInError(pkt) == 0);

	set_header(pkt, 0x100, 0, 1, 3 /* adaptation + payload */, 0, 0);
	CHECK(ltntstools_isPayloadPUSIInError(pkt) == 0);

	set_header(pkt, 0x100, 0, 1, 0 /* reserved */, 0, 0);
	CHECK(ltntstools_isPayloadPUSIInError(pkt) == 1);
}

static void test_notification_event_name_all_values(void)
{
	CHECK(strcmp(ltntstools_notification_event_name(EVENT_UNDEFINED), "EVENT_UNDEFINED") == 0);
	CHECK(strcmp(ltntstools_notification_event_name(EVENT_UPDATE_STREAM_CC_COUNT), "EVENT_UPDATE_STREAM_CC_COUNT") == 0);
	CHECK(strcmp(ltntstools_notification_event_name(EVENT_UPDATE_STREAM_MBPS), "EVENT_UPDATE_STREAM_MBPS") == 0);
	/* Guaranteed to always return a string, even for an out-of-range value. */
	CHECK(ltntstools_notification_event_name((enum ltntstools_notification_event_e)9999) != NULL);
	CHECK(strcmp(ltntstools_notification_event_name((enum ltntstools_notification_event_e)9999), "EVENT_UNKNOWN") == 0);
}

/* -------- notification register / unregister -------- */

static void dummy_cb(void *ctx, enum ltntstools_notification_event_e e,
	const struct ltntstools_stream_statistics_s *s, const struct ltntstools_pid_statistics_s *p)
{
	(void)ctx; (void)e; (void)s; (void)p;
}

static void test_notification_register_rejects_invalid(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	CHECK(ltntstools_notification_register_callback(NULL, EVENT_UPDATE_STREAM_CC_COUNT, NULL, dummy_cb) < 0);
	CHECK(ltntstools_notification_register_callback(s, EVENT_NOTIFICATION_MAX, NULL, dummy_cb) < 0);
	CHECK(ltntstools_notification_register_callback(s, (enum ltntstools_notification_event_e)-1, NULL, dummy_cb) < 0);
	CHECK(ltntstools_notification_register_callback(s, EVENT_UPDATE_STREAM_CC_COUNT, NULL, NULL) < 0);

	/* NULL-safe unregister variants must not crash either. */
	ltntstools_notification_unregister_callback(NULL, EVENT_UPDATE_STREAM_CC_COUNT);
	ltntstools_notification_unregister_callbacks(NULL);

	ltntstools_pid_stats_free(s);
}

struct notify_capture_s {
	int count;
	enum ltntstools_notification_event_e lastEvent;
};

static void capture_cb(void *ctx, enum ltntstools_notification_event_e e,
	const struct ltntstools_stream_statistics_s *s, const struct ltntstools_pid_statistics_s *p)
{
	struct notify_capture_s *cap = (struct notify_capture_s *)ctx;
	cap->count++;
	cap->lastEvent = e;
}

static void test_notification_register_unregister_single(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	struct notify_capture_s cap = { 0 };
	CHECK(ltntstools_notification_register_callback(s, EVENT_UPDATE_STREAM_TEI_COUNT, &cap, capture_cb) == 0);

	uint8_t pkt[188];
	set_header(pkt, 0x60, 1 /* TEI */, 0, 1, 0, 0);
	ltntstools_pid_stats_update(s, pkt, 1);
	CHECK(cap.count == 1);
	CHECK(cap.lastEvent == EVENT_UPDATE_STREAM_TEI_COUNT);

	ltntstools_notification_unregister_callback(s, EVENT_UPDATE_STREAM_TEI_COUNT);
	ltntstools_pid_stats_update(s, pkt, 1); /* another TEI packet, callback now unregistered */
	CHECK(cap.count == 1); /* unchanged */

	ltntstools_pid_stats_free(s);
}

static void test_notification_unregister_all(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	struct notify_capture_s cap = { 0 };
	ltntstools_notification_register_callback(s, EVENT_UPDATE_STREAM_TEI_COUNT, &cap, capture_cb);
	ltntstools_notification_register_callback(s, EVENT_UPDATE_STREAM_SCRAMBLED_COUNT, &cap, capture_cb);

	ltntstools_notification_unregister_callbacks(s);

	uint8_t pkt[188];
	set_header(pkt, 0x60, 1, 0, 1, 2 /* scrambled too */, 0);
	ltntstools_pid_stats_update(s, pkt, 1);
	CHECK(cap.count == 0);

	ltntstools_pid_stats_free(s);
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	struct ltntstools_stream_statistics_s *s = NULL;
	CHECK(ltntstools_pid_stats_alloc(&s) == 0);
	CHECK(s != NULL);
	ltntstools_pid_stats_free(s);
}

static void test_free_null_safe(void)
{
	ltntstools_pid_stats_free(NULL);
	CHECK(1);
}

static void test_get_unknown_pid_returns_null(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);
	CHECK(ltntstools_pid_stats_get(s, 0x123) == NULL);
	ltntstools_pid_stats_free(s);
}

static void test_get_null_stream_returns_null(void)
{
	CHECK(ltntstools_pid_stats_get(NULL, 0x123) == NULL);
}

static void test_reset_zeroes_counters(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkt[188];
	set_header(pkt, 0x50, 0, 0, 1, 0, 0);
	ltntstools_pid_stats_update(s, pkt, 1);
	CHECK(ltntstools_pid_stats_stream_get_packet_count(s) == 1);

	ltntstools_pid_stats_reset(s);
	CHECK(ltntstools_pid_stats_stream_get_packet_count(s) == 0);
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 0);

	ltntstools_pid_stats_free(s);
}

/* -------- core ltntstools_pid_stats_update() -------- */

static void test_packet_count_increments(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkts[7 * 188];
	for (int i = 0; i < 7; i++) {
		set_header(pkts + i * 188, 0x90, 0, 0, 1, 0, 0);
	}
	ltntstools_pid_stats_update(s, pkts, 7);
	CHECK(ltntstools_pid_stats_stream_get_packet_count(s) == 7);
	CHECK(ltntstools_pid_stats_pid_get_packet_count(s, 0x90) == 7);

	ltntstools_pid_stats_free(s);
}

static void test_not_multiple_of_seven_counter(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkts[7 * 188];
	for (int i = 0; i < 7; i++) {
		set_header(pkts + i * 188, 0x90, 0, 0, 1, 0, 0);
	}

	ltntstools_pid_stats_update(s, pkts, 7);
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_errors(s) == 0);

	ltntstools_pid_stats_update(s, pkts, 3);
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_errors(s) == 1);
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_time(s) != 0);

	ltntstools_pid_stats_free(s);
}

static void test_new_pid_discovery(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	CHECK(ltntstools_pid_stats_get(s, 0x200) == NULL);

	uint8_t pkt[188];
	set_header(pkt, 0x200, 0, 0, 1, 0, 0);
	ltntstools_pid_stats_update(s, pkt, 1);

	struct ltntstools_pid_statistics_s *pid = ltntstools_pid_stats_get(s, 0x200);
	CHECK(pid != NULL);
	CHECK(pid->pidNr == 0x200);
	CHECK(pid->enabled == 1);

	ltntstools_pid_stats_free(s);
}

static void test_cc_error_first_packet_immunity(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	/* First-ever packet on a pid, cc=9 (arbitrary): must never count as a CC
	 * error, since there's no prior CC to compare against. */
	uint8_t pkt[188];
	set_header(pkt, 0x300, 0, 0, 1, 0, 9);
	ltntstools_pid_stats_update(s, pkt, 1);

	CHECK(ltntstools_pid_stats_pid_get_cc_errors(s, 0x300) == 0);
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 0);

	ltntstools_pid_stats_free(s);
}

static void test_cc_error_detected_and_notified(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	struct notify_capture_s cap = { 0 };
	ltntstools_notification_register_callback(s, EVENT_UPDATE_STREAM_CC_COUNT, &cap, capture_cb);

	uint8_t buf[2 * 188];
	set_header(buf, 0x50, 0, 0, 1, 0, 0);
	set_header(buf + 188, 0x50, 0, 0, 1, 0, 5); /* cc jumps 0 -> 5 */
	ltntstools_pid_stats_update(s, buf, 2);

	CHECK(ltntstools_pid_stats_pid_get_cc_errors(s, 0x50) == 1);
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 1);
	CHECK(ltntstools_pid_stats_stream_get_cc_error_time(s) != 0);
	CHECK(cap.count == 1);
	CHECK(cap.lastEvent == EVENT_UPDATE_STREAM_CC_COUNT);

	ltntstools_pid_stats_free(s);
}

static void test_cc_error_never_counted_for_null_pid(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t buf[2 * 188];
	set_header(buf, 0x1fff, 0, 0, 1, 0, 0);
	set_header(buf + 188, 0x1fff, 0, 0, 1, 0, 9); /* would be an error on any other pid */
	ltntstools_pid_stats_update(s, buf, 2);

	CHECK(ltntstools_pid_stats_pid_get_cc_errors(s, 0x1fff) == 0);
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 0);

	ltntstools_pid_stats_free(s);
}

static void test_tei_error_detected_and_notified(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	struct notify_capture_s cap = { 0 };
	ltntstools_notification_register_callback(s, EVENT_UPDATE_STREAM_TEI_COUNT, &cap, capture_cb);

	uint8_t pkt[188];
	set_header(pkt, 0x60, 1 /* TEI */, 0, 1, 0, 0);
	ltntstools_pid_stats_update(s, pkt, 1);

	CHECK(ltntstools_pid_stats_pid_get_tei_errors(s, 0x60) == 1);
	CHECK(ltntstools_pid_stats_stream_get_tei_errors(s) == 1);
	CHECK(cap.count == 1);
	CHECK(cap.lastEvent == EVENT_UPDATE_STREAM_TEI_COUNT);

	ltntstools_pid_stats_free(s);
}

static void test_scrambled_detected_and_notified(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	struct notify_capture_s cap = { 0 };
	ltntstools_notification_register_callback(s, EVENT_UPDATE_STREAM_SCRAMBLED_COUNT, &cap, capture_cb);

	uint8_t pkt[188];
	set_header(pkt, 0x70, 0, 0, 1, 2 /* scrambled */, 0);
	ltntstools_pid_stats_update(s, pkt, 1);

	CHECK(ltntstools_pid_stats_stream_get_scrambled_count(s) == 1);
	CHECK(cap.count == 1);
	CHECK(cap.lastEvent == EVENT_UPDATE_STREAM_SCRAMBLED_COUNT);

	ltntstools_pid_stats_free(s);
}

static void test_payload_pusi_error_detected(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkt[188];
	set_header(pkt, 0x80, 0, 1 /* PUSI */, 2 /* adaptation only: illegal combo */, 0, 0);
	ltntstools_pid_stats_update(s, pkt, 1);

	CHECK(ltntstools_pid_stats_stream_get_pusi_payload_errors(s) == 1);
	CHECK(ltntstools_pid_stats_pid_get_pusi_payload_errors(s, 0x80) == 1);

	ltntstools_pid_stats_free(s);
}

static void test_first_update_call_pps_quirk(void)
{
	/* Deterministic consequence of the once-per-wall-second pps publish:
	 * pps_last_update starts at 0, so the very first update() call always
	 * publishes an (empty) window, and pps reads 0 immediately after it --
	 * even though packets were just processed. */
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkts[7 * 188];
	for (int i = 0; i < 7; i++) {
		set_header(pkts + i * 188, 0x90, 0, 0, 1, 0, 0);
	}
	ltntstools_pid_stats_update(s, pkts, 7);

	CHECK(ltntstools_pid_stats_stream_get_pps(s) == 0);
	CHECK(ltntstools_pid_stats_stream_get_packet_count(s) == 7);

	ltntstools_pid_stats_free(s);
}

/* -------- clone() -------- */

static void test_clone_null_source_returns_null(void)
{
	CHECK(ltntstools_pid_stats_clone(NULL) == NULL);
}

static void test_clone_matches_source(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkts[7 * 188];
	for (int i = 0; i < 7; i++) {
		set_header(pkts + i * 188, 0x90, 0, 0, 1, 0, 0);
	}
	ltntstools_pid_stats_update(s, pkts, 7);

	struct ltntstools_stream_statistics_s *clone = ltntstools_pid_stats_clone(s);
	CHECK(clone != NULL);
	CHECK(ltntstools_pid_stats_stream_get_packet_count(clone) == ltntstools_pid_stats_stream_get_packet_count(s));
	CHECK(ltntstools_pid_stats_pid_get_packet_count(clone, 0x90) == ltntstools_pid_stats_pid_get_packet_count(s, 0x90));

	ltntstools_pid_stats_free(s);
	ltntstools_pid_stats_free(clone);
}

static void test_clone_independent_of_source_mutation(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkts[7 * 188];
	for (int i = 0; i < 7; i++) {
		set_header(pkts + i * 188, 0x90, 0, 0, 1, 0, 0);
	}
	ltntstools_pid_stats_update(s, pkts, 7);

	struct ltntstools_stream_statistics_s *clone = ltntstools_pid_stats_clone(s);

	ltntstools_pid_stats_update(s, pkts, 7); /* mutate the original after cloning */

	CHECK(ltntstools_pid_stats_stream_get_packet_count(s) == 14);
	CHECK(ltntstools_pid_stats_stream_get_packet_count(clone) == 7);

	ltntstools_pid_stats_free(s);
	ltntstools_pid_stats_free(clone);
}

/* -------- PCR tracking / bitrate calculator -------- */

static void feed_pcr(struct ltntstools_stream_statistics_s *s, uint8_t *cc, uint64_t pcr)
{
	uint8_t pkt[188];
	ltntstools_generatePCROnlyPacket(pkt, sizeof(pkt), 0x100, cc, pcr);
	ltntstools_pid_stats_update(s, pkt, 1);
}

static void test_contains_pcr_set_get(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	CHECK(ltntstools_pid_stats_pid_get_contains_pcr(s, 0x100) == 0);
	ltntstools_pid_stats_pid_set_contains_pcr(s, 0x100);
	CHECK(ltntstools_pid_stats_pid_get_contains_pcr(s, 0x100) == 1);

	ltntstools_pid_stats_free(s);
}

/* seenPCR must exceed 100 before the PCR clock (and get_pcr()) become live --
 * confirmed against the real implementation before hard-coding these numbers. */
static void test_get_pcr_before_and_after_warmup(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);
	ltntstools_pid_stats_pid_set_contains_pcr(s, 0x100);

	uint8_t cc = 0;
	uint64_t pcr = 0;
	for (int i = 0; i < 100; i++) {
		feed_pcr(s, &cc, pcr);
		pcr += 90000; /* 3.33ms apart, well under the 40ms violation threshold */
	}
	CHECK(ltntstools_pid_stats_pid_get_pcr(s, 0x100) == 0); /* still warming up */

	feed_pcr(s, &cc, pcr); /* 101st PCR-bearing packet */
	CHECK(ltntstools_pid_stats_pid_get_pcr(s, 0x100) == (int64_t)pcr);

	ltntstools_pid_stats_free(s);
}

static void test_pcr_violate_timing_progression(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);
	ltntstools_pid_stats_pid_set_contains_pcr(s, 0x100);

	uint8_t cc = 0;
	uint64_t pcr = 0;
	for (int i = 0; i < 102; i++) {
		feed_pcr(s, &cc, pcr);
		pcr += 90000;
	}
	CHECK(ltntstools_pid_stats_pid_did_violate_pcr_timing(s, 0x100) == 0);
	CHECK(ltntstools_pid_stats_stream_did_violate_pcr_timing(s) == 0);

	pcr += 27000 * 50; /* 50ms gap: exceeds the 40ms threshold */
	feed_pcr(s, &cc, pcr);
	CHECK(ltntstools_pid_stats_pid_did_violate_pcr_timing(s, 0x100) == 1);
	CHECK(ltntstools_pid_stats_stream_did_violate_pcr_timing(s) == 1);

	ltntstools_pid_stats_free(s);
}

static void test_pcr_walltime_drift_getter(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	int64_t drift = -999;
	/* Pid not yet marked as a PCR pid: must fail cleanly. */
	CHECK(ltntstools_pid_stats_pid_get_pcr_walltime_driftms(s, 0x100, &drift) < 0);

	ltntstools_pid_stats_pid_set_contains_pcr(s, 0x100);
	uint8_t cc = 0;
	uint64_t pcr = 0;
	for (int i = 0; i < 101; i++) {
		feed_pcr(s, &cc, pcr);
		pcr += 90000;
	}
	CHECK(ltntstools_pid_stats_pid_get_pcr_walltime_driftms(s, 0x100, &drift) == 0);

	/* NULL out-param must be rejected. */
	CHECK(ltntstools_pid_stats_pid_get_pcr_walltime_driftms(s, 0x100, NULL) < 0);

	ltntstools_pid_stats_free(s);
}

static void test_bitrate_calculator_query_values(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);
	ltntstools_pid_stats_pid_set_contains_pcr(s, 0x100);

	uint8_t buf[5 * 188];
	uint8_t cc = 0;
	ltntstools_generatePCROnlyPacket(buf + 0 * 188, 188, 0x100, &cc, 0);
	set_header(buf + 1 * 188, 0x200, 0, 0, 1, 0, 0);
	set_header(buf + 2 * 188, 0x300, 0, 0, 1, 0, 0);
	set_header(buf + 3 * 188, 0x400, 0, 0, 1, 0, 0);
	/* Second PCR exactly 1 second (27,000,000 ticks @ 27MHz) later. */
	ltntstools_generatePCROnlyPacket(buf + 4 * 188, 188, 0x100, &cc, 27000000ULL);

	ltntstools_pid_stats_update(s, buf, 5);

	double bps = -1;
	int64_t ticks = -1, stc = -1;
	CHECK(ltntstools_bitrate_calculator_query_bitrate(s, &bps) == 0);
	CHECK(ltntstools_bitrate_calculator_query_ticks_per_packet(s, &ticks) == 0);
	CHECK(ltntstools_bitrate_calculator_query_stc(s, &stc) == 0);

	/* 4 packets between the two PCRs, 1 second apart -> 4*188*8 = 6016 bps. */
	CHECK(bps == 6016.0);
	CHECK(ticks == 6750000);
	CHECK(stc == 27000000);
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 0);

	CHECK(ltntstools_bitrate_calculator_query_bitrate(NULL, &bps) < 0);
	CHECK(ltntstools_bitrate_calculator_query_bitrate(s, NULL) < 0);

	ltntstools_pid_stats_free(s);
}

/* -------- CTP stats -------- */

static void test_ctp_first_call_no_cc_error(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t buf[7 * 188] = { 0 };
	buf[2] = 0x00; buf[3] = 0x05; /* arbitrary first sequence number */
	ltntstools_ctp_stats_update(s, buf, sizeof(buf));

	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 0);
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_errors(s) == 0);

	ltntstools_pid_stats_free(s);
}

static void test_ctp_sequence_skip_detected(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t buf[7 * 188] = { 0 };
	buf[2] = 0x00; buf[3] = 0x05;
	ltntstools_ctp_stats_update(s, buf, sizeof(buf));

	buf[3] = 0x06; /* correct next sequence: no error */
	ltntstools_ctp_stats_update(s, buf, sizeof(buf));
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 0);

	buf[3] = 0x0a; /* skipped ahead: error */
	ltntstools_ctp_stats_update(s, buf, sizeof(buf));
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 1);

	ltntstools_pid_stats_free(s);
}

static void test_ctp_malformed_buffer_ignored_but_counted(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t small[2] = { 0, 0 };
	ltntstools_ctp_stats_update(s, small, sizeof(small));

	/* lengthBytes < 4 is ignored for CC/packet-count purposes... */
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(s) == 0);
	CHECK(ltntstools_pid_stats_stream_get_packet_count(s) == 0);
	/* ...but the not-multiple-of-7*188 check runs before that early-return. */
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_errors(s) == 1);

	ltntstools_pid_stats_free(s);
}

/* -------- bytestream stats -------- */

static void test_bytestream_not_multiple_of_seven(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t buf[7 * 188] = { 0 };
	ltntstools_bytestream_stats_update(s, buf, sizeof(buf));
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_errors(s) == 0);

	ltntstools_bytestream_stats_update(s, buf, 100);
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_errors(s) == 1);

	ltntstools_pid_stats_free(s);
}

/* -------- NULL-stream getters: documented to return safe defaults -------- */

static void test_null_stream_getters_return_safe_defaults(void)
{
	CHECK(ltntstools_ctp_stats_stream_get_mbps(NULL) == 0);
	CHECK(ltntstools_bytestream_stats_stream_get_mbps(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_mbps(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_pps(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_bps(NULL) == 0);
	CHECK(ltntstools_ctp_stats_stream_get_bps(NULL) == 0);
	CHECK(ltntstools_bytestream_stats_stream_get_bps(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_cc_errors(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_tei_errors(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_cc_error_time(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_errors(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_notmultipleofseven_time(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_get_scrambled_count(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_padding_pct(NULL) == 0);
	CHECK(ltntstools_pid_stats_stream_did_violate_pcr_timing(NULL) != 0); /* documented: nonzero on NULL */
	CHECK(ltntstools_pid_stats_stream_get_packet_count(NULL) == 0);
	CHECK(ltntstools_pid_stats_pid_get_pusi_payload_errors(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_stream_get_pusi_payload_errors(NULL) == 0);
	CHECK(ltntstools_pid_stats_pid_get_mbps(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_get_pps(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_get_bps(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_get_packet_count(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_get_cc_errors(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_get_tei_errors(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_get_last_update(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_did_violate_pcr_timing(NULL, 0x100) != 0);
	CHECK(ltntstools_pid_stats_pid_get_contains_pcr(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_pid_get_pcr(NULL, 0x100) == 0);
	CHECK(ltntstools_pid_stats_stream_get_iat_hwm_us(NULL) == 0);

	int64_t drift;
	CHECK(ltntstools_pid_stats_pid_get_pcr_walltime_driftms(NULL, 0x100, &drift) < 0);

	double bps;
	int64_t v;
	CHECK(ltntstools_bitrate_calculator_query_bitrate(NULL, &bps) < 0);
	CHECK(ltntstools_bitrate_calculator_query_ticks_per_packet(NULL, &v) < 0);
	CHECK(ltntstools_bitrate_calculator_query_stc(NULL, &v) < 0);

	/* Must also not crash: */
	ltntstools_pid_stats_update(NULL, (const uint8_t *)"x", 1);
	ltntstools_pid_stats_reset(NULL);
	ltntstools_pid_stats_pid_set_contains_pcr(NULL, 0x100);
	ltntstools_pid_stats_dprintf(NULL, 1);
}

/* -------- dprintf smoke test -------- */

static void test_dprintf_does_not_crash(void)
{
	struct ltntstools_stream_statistics_s *s;
	ltntstools_pid_stats_alloc(&s);

	uint8_t pkt[188];
	set_header(pkt, 0x50, 0, 0, 1, 0, 0);
	ltntstools_pid_stats_update(s, pkt, 1);

	int fd = open("/dev/null", 1 /* O_WRONLY */);
	if (fd >= 0) {
		ltntstools_pid_stats_dprintf(s, fd);
		close(fd);
	}
	CHECK(1);

	ltntstools_pid_stats_free(s);
}

int main(void)
{
	test_isCCInError_adaptation_only_or_reserved();
	test_isCCInError_payload_bearing();
	test_isCCInError_wraparound();
	test_isPayloadPUSIInError();
	test_notification_event_name_all_values();

	test_notification_register_rejects_invalid();
	test_notification_register_unregister_single();
	test_notification_unregister_all();

	test_alloc_free_basic();
	test_free_null_safe();
	test_get_unknown_pid_returns_null();
	test_get_null_stream_returns_null();
	test_reset_zeroes_counters();

	test_packet_count_increments();
	test_not_multiple_of_seven_counter();
	test_new_pid_discovery();
	test_cc_error_first_packet_immunity();
	test_cc_error_detected_and_notified();
	test_cc_error_never_counted_for_null_pid();
	test_tei_error_detected_and_notified();
	test_scrambled_detected_and_notified();
	test_payload_pusi_error_detected();
	test_first_update_call_pps_quirk();

	test_clone_null_source_returns_null();
	test_clone_matches_source();
	test_clone_independent_of_source_mutation();

	test_contains_pcr_set_get();
	test_get_pcr_before_and_after_warmup();
	test_pcr_violate_timing_progression();
	test_pcr_walltime_drift_getter();
	test_bitrate_calculator_query_values();

	test_ctp_first_call_no_cc_error();
	test_ctp_sequence_skip_detected();
	test_ctp_malformed_buffer_ignored_but_counted();

	test_bytestream_not_multiple_of_seven();

	test_null_stream_getters_return_safe_defaults();
	test_dprintf_does_not_crash();

	if (g_failures == 0) {
		printf("PASS: all stats tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d stats test(s) failed\n", g_failures);
	return 1;
}
