/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Three real bugs were found and fixed in src/pes-extractor.c while writing
 * this file's newest tests (confirmed with AddressSanitizer before the fix,
 * re-confirmed clean after):
 *  1. *** Critical, remotely triggerable from raw/untrusted TS input ***
 *     ltntstools_pes_extractor_write() computed the payload offset as
 *     `4 + 1 + ltntstools_adaption_field_length(pkt)` with no bounds check.
 *     adaptation_field_length is a raw, attacker-controlled byte (0-255);
 *     a single malformed packet (adaptation_field_control = 0b11, length
 *     byte = 255) drives that offset past 188, making `wsize = 188 -
 *     offset` negative -- which becomes a huge size_t once handed to
 *     rb_write_with_state(). Confirmed: a single such packet produced a
 *     ~4MB stack-buffer-underflow read starting from a wild pointer
 *     (pkt + offset, itself already past the 188-byte packet). Fixed by
 *     clamping offset to at most 188 right after it's computed.
 *     test_write_malformed_adaptation_field_length_does_not_crash() below
 *     is the regression test.
 *  2. ltntstools_pes_extractor_alloc(): ctx->rb = rb_new(buffer_min,
 *     buffer_max) was never NULL-checked. rb_new() returns NULL whenever
 *     buffer_min == 0 or buffer_min > buffer_max; alloc() still reported
 *     success, and the first write() dereferenced the NULL ctx->rb.
 *     test_alloc_rejects_invalid_buffer_range() below is the regression
 *     test.
 *  3. _processRing(): ltn_pes_packet_alloc() and pes->rawBuffer = malloc()
 *     were both used without a NULL check before being dereferenced /
 *     memcpy()'d into (OOM-only, not independently regression tested here).
 *
 * Unit tests for src/pes-extractor.c / src/libltntstools/pes-extractor.h.
 * Builds against ../src/pes-extractor.c plus its dependencies:
 * ../src/klringbuffer.c (ring buffer), ../src/utils.c (pulled in via
 * "utils.h"), ../src/pes.c/../src/crc32.c (used to build+pack real PES
 * buffers for feeding into the extractor), and ../src/stats.c plus its own
 * ../src/clocks.c/../src/ts.c/../src/history-metric.c dependencies
 * (pes-extractor.c drives a full struct ltntstools_stream_statistics_s to
 * track CC errors and derive a synthesized PCR/STC). Links -lpthread
 * (stats.c and pes-extractor.c both use pthread mutexes).
 *
 * Framing note: ltntstools_pes_extractor_write() only ever hands a PES to
 * the callback when the *next* payload_unit_start_indicator=1 TS packet for
 * the same pid arrives (that's what triggers _processRing() on the
 * previously-accumulated ring). So every test below appends one extra
 * "trailer" TS packet (PUSI=1, throwaway single-byte payload) after the
 * real PES packet(s) purely to flush it through -- its own content is never
 * asserted on.
 *
 * DOC DISCREPANCY (not fixed, since it's the header comment that's wrong,
 * not the code -- see ltntstools_pes_extractor_set_skip_data()'s doc in
 * pes-extractor.h): the header claims tf=1 means "add data" and the
 * default (before ever calling this) is "not attached". The real field is
 * literally named skipDataExtraction and is passed straight through as
 * ltn_pes_packet_parse()'s `skipData` argument, where 1 means *skip*
 * (don't attach) payload data -- the opposite of the header's claim. Since
 * the struct is calloc()'d, skipDataExtraction starts at 0, so payload data
 * IS attached by default. test_write_default_attaches_data_test_skip_data_
 * true_omits_it below pins down the real (not documented) behavior.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* pes.h must precede pes-extractor.h: pes-extractor.h's callback typedef
 * uses struct ltn_pes_packet_s without declaring/including it, so it needs
 * the real definition already in scope for capture_cb's signature below to
 * match pes_extractor_callback (an opaque forward decl there is a distinct,
 * incompatible type from the real struct). */
#include "libltntstools/pes.h"
#include "libltntstools/pes-extractor.h"
#include "libltntstools/klbitstream_readwriter.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- building a real, valid, parseable PES byte buffer --------
 * Mirrors test_pes.c's pack_with_correct_length(): pack() writes whatever
 * PES_packet_length is already in the struct rather than computing it, so
 * pack once, then patch the length field bytes to match pack()'s own
 * returned byte count.
 */
static int pack_pes_with_pts(uint8_t streamId, uint32_t ptsDtsFlags, int64_t pts,
	const uint8_t *payload, int payloadLen, uint8_t *buf, int bufLen)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = streamId;
	pkt.data_alignment_indicator = 1;
	pkt.PTS_DTS_flags = ptsDtsFlags;
	pkt.PTS = pts;
	pkt.data = (unsigned char *)payload;
	pkt.dataLengthBytes = payloadLen;

	memset(buf, 0, bufLen);
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_write_set_buffer(&bs, buf, bufLen);

	ssize_t bits = ltn_pes_packet_pack(&pkt, &bs);
	int totalBytes = (int)(bits / 8);
	uint16_t correctLength = (uint16_t)(totalBytes - 6);
	buf[4] = (correctLength >> 8) & 0xff;
	buf[5] = correctLength & 0xff;

	return totalBytes;
}

static int pack_pes(uint8_t streamId, const uint8_t *payload, int payloadLen, uint8_t *buf, int bufLen)
{
	return pack_pes_with_pts(streamId, 0, 0, payload, payloadLen, buf, bufLen);
}

/* Splits `buf` into 188-byte TS packets for `pid`, PUSI=1 on the first
 * packet only, continuity counter threaded through *cc across calls so a
 * whole test's packet stream stays contiguous. Returns packets written. */
static int build_ts_packets(const uint8_t *buf, int bufLen, uint16_t pid, uint8_t *cc,
	uint8_t packets[][188], int maxPackets)
{
	int n = 0;
	int offset = 0;
	while (offset < bufLen && n < maxPackets) {
		uint8_t *pkt = packets[n];
		memset(pkt, 0xFF, 188);
		pkt[0] = 0x47;
		pkt[1] = ((offset == 0) ? 0x40 : 0x00) | ((pid >> 8) & 0x1f);
		pkt[2] = pid & 0xff;
		pkt[3] = 0x10 | (*cc & 0x0f);
		*cc = (*cc + 1) & 0x0f;

		int chunk = bufLen - offset;
		if (chunk > 184)
			chunk = 184;
		memcpy(&pkt[4], buf + offset, chunk);

		offset += chunk;
		n++;
	}
	return n;
}

/* One throwaway PUSI=1 packet on `pid`: flushes whatever's currently
 * accumulating in the ring by triggering _processRing() on it. Its own
 * content is never a real PES and is never itself flushed/asserted on. */
static void build_trailer_packet(uint16_t pid, uint8_t *cc, uint8_t pkt[188])
{
	memset(pkt, 0xFF, 188);
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((pid >> 8) & 0x1f);
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10 | (*cc & 0x0f);
	*cc = (*cc + 1) & 0x0f;
	pkt[4] = 0x00;
}

/* -------- callback capture -------- */

#define MAX_CAPTURED 16
static struct ltn_pes_packet_s *g_captured[MAX_CAPTURED];
static int g_capturedCount;

static void reset_capture(void)
{
	g_capturedCount = 0;
	memset(g_captured, 0, sizeof(g_captured));
}

static void capture_cb(void *userContext, struct ltn_pes_packet_s *pes)
{
	if (g_capturedCount < MAX_CAPTURED) {
		g_captured[g_capturedCount++] = pes;
	} else {
		ltn_pes_packet_free(pes);
	}
}

static void free_captured(void)
{
	for (int i = 0; i < g_capturedCount; i++) {
		if (g_captured[i])
			ltn_pes_packet_free(g_captured[i]);
	}
	reset_capture();
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1) == 0);
	CHECK(hdl != NULL);
	ltntstools_pes_extractor_free(hdl);
}

static void test_setters_return_success(void)
{
	void *hdl = NULL;
	ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1);

	CHECK(ltntstools_pes_extractor_set_skip_data(hdl, 1) == 0);
	CHECK(ltntstools_pes_extractor_set_ordered_output(hdl, 0) == 0);
	CHECK(ltntstools_pes_extractor_set_pcr_pid(hdl, 0x200) == 0);

	ltntstools_pes_extractor_free(hdl);
}

/* -------- write() pid filtering -------- */

static void test_write_ignores_other_pid_no_callback(void)
{
	void *hdl = NULL;
	ltntstools_pes_extractor_alloc(&hdl, 0x100 /* target */, 0xE0, capture_cb, NULL, -1, -1);
	reset_capture();

	uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t buf[64];
	int totalBytes = pack_pes(0xE0, payload, sizeof(payload), buf, sizeof(buf));

	uint8_t packets[4][188];
	uint8_t cc = 0;
	int n = build_ts_packets(buf, totalBytes, 0x200 /* NOT the target pid */, &cc, packets, 4);
	build_trailer_packet(0x200, &cc, packets[n++]);

	ssize_t ret = ltntstools_pes_extractor_write(hdl, &packets[0][0], n);
	CHECK(ret == n);
	CHECK(g_capturedCount == 0);

	ltntstools_pes_extractor_free(hdl);
}

/* -------- write() single-packet PES happy path -------- */

static void test_write_default_attaches_data_test_skip_data_true_omits_it(void)
{
	uint8_t payload[10] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33 };

	for (int skip = 0; skip <= 1; skip++) {
		void *hdl = NULL;
		ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1);
		ltntstools_pes_extractor_set_skip_data(hdl, skip);
		reset_capture();

		uint8_t buf[64];
		int totalBytes = pack_pes(0xE0, payload, sizeof(payload), buf, sizeof(buf));

		uint8_t packets[4][188];
		uint8_t cc = 0;
		int n = build_ts_packets(buf, totalBytes, 0x100, &cc, packets, 4);
		build_trailer_packet(0x100, &cc, packets[n++]);

		ltntstools_pes_extractor_write(hdl, &packets[0][0], n);

		CHECK(g_capturedCount == 1);
		if (g_capturedCount == 1) {
			struct ltn_pes_packet_s *pes = g_captured[0];
			CHECK(pes->stream_id == 0xE0);
			if (skip == 0) {
				CHECK(pes->dataLengthBytes == sizeof(payload));
				CHECK(pes->data != NULL && memcmp(pes->data, payload, sizeof(payload)) == 0);
			} else {
				CHECK(pes->data == NULL);
			}
		}

		free_captured();
		ltntstools_pes_extractor_free(hdl);
	}
}

/* -------- write() large PES spanning multiple TS packets -------- */

static void test_write_large_pes_spanning_multiple_ts_packets_reassembles(void)
{
	void *hdl = NULL;
	ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1);
	reset_capture();

	uint8_t payload[400];
	for (int i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (uint8_t)i;

	uint8_t buf[512];
	int totalBytes = pack_pes(0xE0, payload, sizeof(payload), buf, sizeof(buf));
	CHECK(totalBytes > 184); /* sanity: must actually span >1 TS packet */

	uint8_t packets[8][188];
	uint8_t cc = 0;
	int n = build_ts_packets(buf, totalBytes, 0x100, &cc, packets, 8);
	CHECK(n > 1);
	build_trailer_packet(0x100, &cc, packets[n++]);

	ltntstools_pes_extractor_write(hdl, &packets[0][0], n);

	CHECK(g_capturedCount == 1);
	if (g_capturedCount == 1) {
		struct ltn_pes_packet_s *pes = g_captured[0];
		CHECK(pes->dataLengthBytes == sizeof(payload));
		CHECK(pes->data != NULL && memcmp(pes->data, payload, sizeof(payload)) == 0);
	}

	free_captured();
	ltntstools_pes_extractor_free(hdl);
}

/* -------- write() CC discontinuity discards the partial PES -------- */

static void test_write_cc_error_discards_partial_pes_but_recovers(void)
{
	void *hdl = NULL;
	ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1);
	reset_capture();

	/* A PES big enough to need 2 TS packets, so there's a "mid-pes"
	 * continuation packet whose CC we can corrupt. */
	uint8_t payload1[300];
	memset(payload1, 0xAB, sizeof(payload1));
	uint8_t buf1[512];
	int totalBytes1 = pack_pes(0xE0, payload1, sizeof(payload1), buf1, sizeof(buf1));

	uint8_t payload2[20];
	memset(payload2, 0x5A, sizeof(payload2));
	uint8_t buf2[64];
	int totalBytes2 = pack_pes(0xE0, payload2, sizeof(payload2), buf2, sizeof(buf2));

	uint8_t packets[8][188];
	uint8_t cc = 0;
	int n = build_ts_packets(buf1, totalBytes1, 0x100, &cc, packets, 8);
	CHECK(n >= 2); /* sanity: corrupting packets[1]'s CC requires a 2nd packet */

	/* Introduce a continuity gap on the 2nd packet of PES #1 -- skip a CC
	 * value, simulating a dropped transport packet. */
	packets[1][3] = 0x10 | (((packets[1][3] & 0x0f) + 1) & 0x0f);

	n += build_ts_packets(buf2, totalBytes2, 0x100, &cc, packets + n, 8 - n);
	build_trailer_packet(0x100, &cc, packets[n++]);

	ltntstools_pes_extractor_write(hdl, &packets[0][0], n);

	/* Only the second, uncorrupted PES should have been delivered -- the
	 * framework's core guarantee is that it never hands a mangled PES to
	 * the callback. */
	CHECK(g_capturedCount == 1);
	if (g_capturedCount == 1) {
		struct ltn_pes_packet_s *pes = g_captured[0];
		CHECK(pes->dataLengthBytes == sizeof(payload2));
		CHECK(pes->data != NULL && memcmp(pes->data, payload2, sizeof(payload2)) == 0);
	}

	free_captured();
	ltntstools_pes_extractor_free(hdl);
}

/* -------- ordered output mode -------- */

static void test_ordered_output_defers_until_free_flush(void)
{
	void *hdl = NULL;
	ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1);
	ltntstools_pes_extractor_set_ordered_output(hdl, 1);
	reset_capture();

	uint8_t packets[8][188];
	uint8_t cc = 0;
	int n = 0;

	/* Real, distinct, increasing PTS values -- ordered-output sorts by
	 * corrected PTS, and the still-empty list slots default to a
	 * correctedPTS of 0. Leaving these at 0 (as other tests' PES's do,
	 * since they don't care about PTS) would tie against that same
	 * sentinel and let the reordering logic treat an already-real,
	 * already-delivered-eligible item as "oldest" ahead of a genuinely
	 * still-empty slot, defeating the buffering this test means to
	 * exercise. */
	uint8_t payloadA[6] = { 'A', 'A', 'A', 'A', 'A', 'A' };
	uint8_t bufA[64];
	int lenA = pack_pes_with_pts(0xE0, 2 /* PTS only */, 90000, payloadA, sizeof(payloadA), bufA, sizeof(bufA));
	n += build_ts_packets(bufA, lenA, 0x100, &cc, packets + n, 8 - n);

	uint8_t payloadB[6] = { 'B', 'B', 'B', 'B', 'B', 'B' };
	uint8_t bufB[64];
	int lenB = pack_pes_with_pts(0xE0, 2 /* PTS only */, 180000, payloadB, sizeof(payloadB), bufB, sizeof(bufB));
	n += build_ts_packets(bufB, lenB, 0x100, &cc, packets + n, 8 - n);

	build_trailer_packet(0x100, &cc, packets[n++]);

	ltntstools_pes_extractor_write(hdl, &packets[0][0], n);

	/* The ordered-output list is ORDERED_LIST_DEPTH (60) deep, so with
	 * only 2 real PES's processed, both just occupy previously-empty
	 * placeholder slots -- nothing is old/full enough to evict a real PES
	 * out to the callback yet. */
	CHECK(g_capturedCount == 0);

	/* Freeing must flush whatever's still cached rather than silently
	 * dropping it (the module's own stated purpose for ordered mode). */
	ltntstools_pes_extractor_free(hdl);
	CHECK(g_capturedCount == 2);

	free_captured();
}

/* -------- lifecycle: invalid buffer range -------- */

/* Regression test for bug #2: buffer_min > buffer_max makes rb_new() return
 * NULL internally; alloc() must fail cleanly rather than reporting success
 * with a poisoned handle. */
static void test_alloc_rejects_invalid_buffer_range(void)
{
	void *hdl = (void *)0x1; /* sentinel: alloc() must not leave this untouched-but-claim-success */
	int ret = ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, 1000, 10);
	CHECK(ret < 0);
}

/* -------- write(): adaptation field handling -------- */

/* Regression test for bug #1 (critical): a single packet whose
 * adaptation_field_control claims adaptation+payload and whose
 * adaptation_field_length is the maximum possible value (255) leaves no
 * room for any payload at all within the 188-byte packet. Before the fix,
 * this drove the payload offset past 188 and crashed
 * (AddressSanitizer: stack-buffer-underflow in rb_write_with_state(),
 * called from ltntstools_pes_extractor_write()). The only correct,
 * safe behavior is to treat it as carrying no payload -- write() must
 * simply not crash and must still report packetCount handled. */
static void test_write_malformed_adaptation_field_length_does_not_crash(void)
{
	void *hdl = NULL;
	ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1);
	reset_capture();

	uint8_t pkt[188];
	memset(pkt, 0xFF, sizeof(pkt));
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((0x100 >> 8) & 0x1f); /* PUSI = 1 */
	pkt[2] = 0x100 & 0xff;
	pkt[3] = 0x30; /* adaptation_field_control = 0b11 (adaptation + payload) */
	pkt[4] = 0xFF; /* adaptation_field_length = 255: claims more space than the packet has */

	ssize_t ret = ltntstools_pes_extractor_write(hdl, pkt, 1);
	CHECK(ret == 1);
	CHECK(g_capturedCount == 0); /* no payload bytes existed to form a PES from */

	free_captured();
	ltntstools_pes_extractor_free(hdl);
}

/* A legitimate, small adaptation field on the leading TS packet of a PES
 * must still parse correctly -- proves the bug #1 clamp doesn't break
 * normal, well-formed adaptation-field usage. */
static void test_write_legitimate_adaptation_field_still_reassembles(void)
{
	void *hdl = NULL;
	ltntstools_pes_extractor_alloc(&hdl, 0x100, 0xE0, capture_cb, NULL, -1, -1);
	reset_capture();

	uint8_t payload[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	uint8_t buf[64];
	int totalBytes = pack_pes(0xE0, payload, sizeof(payload), buf, sizeof(buf));

	uint8_t packets[4][188];
	uint8_t cc = 0;
	int n = build_ts_packets(buf, totalBytes, 0x100, &cc, packets, 4);

	/* Insert a legitimate 2-byte adaptation field (stuffing only, no
	 * flags) ahead of the leading packet's PES bytes, shifting them over
	 * within the same 188-byte packet. */
	uint8_t *lead = packets[0];
	uint8_t payloadPortion[184];
	memcpy(payloadPortion, lead + 4, sizeof(payloadPortion));
	lead[3] = 0x30 | (lead[3] & 0x0f); /* adaptation_field_control = 0b11 */
	lead[4] = 1;                       /* adaptation_field_length = 1 */
	lead[5] = 0x00;                    /* 1 stuffing/flags byte */
	memcpy(lead + 6, payloadPortion, sizeof(payloadPortion) - 2);

	build_trailer_packet(0x100, &cc, packets[n++]);

	ltntstools_pes_extractor_write(hdl, &packets[0][0], n);

	CHECK(g_capturedCount == 1);
	if (g_capturedCount == 1) {
		struct ltn_pes_packet_s *pes = g_captured[0];
		CHECK(pes->dataLengthBytes == sizeof(payload));
		CHECK(pes->data != NULL && memcmp(pes->data, payload, sizeof(payload)) == 0);
	}

	free_captured();
	ltntstools_pes_extractor_free(hdl);
}

int main(void)
{
	test_alloc_free_basic();
	test_setters_return_success();
	test_alloc_rejects_invalid_buffer_range();

	test_write_ignores_other_pid_no_callback();
	test_write_default_attaches_data_test_skip_data_true_omits_it();
	test_write_large_pes_spanning_multiple_ts_packets_reassembles();
	test_write_cc_error_discards_partial_pes_but_recovers();
	test_write_malformed_adaptation_field_length_does_not_crash();
	test_write_legitimate_adaptation_field_still_reassembles();

	test_ordered_output_defers_until_free_flush();

	if (g_failures == 0) {
		printf("PASS: all pes-extractor tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d pes-extractor test(s) failed\n", g_failures);
	return 1;
}
