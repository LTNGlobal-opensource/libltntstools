/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/rtp-analyzer.c / src/libltntstools/rtp-analyzer.h.
 * Builds against ../src/rtp-analyzer.c only -- histogram.h and ts.h's
 * ltntstools_pts_diff() are header-only, no other .c dependency.
 *
 * struct rtp_hdr's `seq`/`ts`/`ssrc` fields are read internally via
 * ntohs()/ntohl(), so this file always writes them with htons()/htonl()
 * -- since the round trip (ntohX(htonX(v)) == v) holds on any host, the
 * tests below are correct regardless of this machine's endianness or
 * exactly how the compiler packs the bitfields, without needing to
 * reverse-engineer that packing.
 *
 * CALLING CONVENTION NOTE: rtp_frame_queryPositions()'s `ssrc` parameter is
 * compared directly against the raw wire-order struct field
 * (`hdr->ssrc != ssrc`, no ntohl()) -- confirmed by its one real caller,
 * smoother-rtp.c, which stores `ctx->expectedSSRC = hdr->ssrc` (also
 * un-converted) and passes that straight through. So every ssrc argument
 * below is htonl()'d, same as the wire bytes rtp_frame_queryPositions()
 * itself reinterprets -- passing a host-order value here would silently
 * never match.
 *
 * TWO REAL BUGS PINNED HERE (not fixed, just documented so a future
 * change is a deliberate decision):
 *
 *  1. rtp_hdr_write()'s illegalTSCounterMovementEvents check can never
 *     fire. It calls rtp_hdr_is_continious(ctx, hdr) (which has the side
 *     effect `ctx->last = *hdr;`) and only *afterwards* compares
 *     `ntohs(hdr->seq) < ntohs(ctx->last.seq)` -- but by that point
 *     ctx->last.seq already equals hdr->seq (just overwritten), so the
 *     comparison is always seq < seq, always false. Confirmed via a
 *     standalone repro: writing seq 10 then seq 5 (a genuine backward
 *     jump) leaves illegalTSCounterMovementEvents at 0.
 *     See test_write_illegal_ts_counter_movement_never_fires().
 *
 *  2. rtp_hdr_is_continious() uses `ctx->last.seq > 0` (nonzero) as its
 *     "is this the very first packet" sentinel, rather than a dedicated
 *     flag. A real stream where the 16-bit sequence number legitimately
 *     wraps to exactly 0 leaves ctx->last.seq == 0 after that packet, so
 *     the *following* packet's continuity check is silently skipped
 *     (treated as if it were the first packet ever), even if it's wildly
 *     discontinuous. Confirmed via a standalone repro: seq 0 followed by
 *     seq 500 reports match=1 (continuous).
 *     See test_is_continious_seq_zero_sentinel_swallows_next_check().
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <arpa/inet.h>

#include "libltntstools/rtp-analyzer.h"

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

static struct rtp_hdr make_hdr(uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc, int m)
{
	struct rtp_hdr h;
	memset(&h, 0, sizeof(h));
	h.version = 2;
	h.pt = pt;
	h.seq = htons(seq);
	h.ts = htonl(ts);
	h.ssrc = htonl(ssrc);
	h.m = m ? 1 : 0;
	return h;
}

/* ---------------------------------------------------------------------
 * rtp_hdr_is_payload_type_valid()
 * --------------------------------------------------------------------- */

static void test_payload_type_valid(void)
{
	struct rtp_hdr h;

	h = make_hdr(95, 0, 0, 0, 0);
	CHECK(rtp_hdr_is_payload_type_valid(&h) == 0); /* just below dynamic range */

	h = make_hdr(96, 0, 0, 0, 0);
	CHECK(rtp_hdr_is_payload_type_valid(&h) == 1); /* dynamic range low end */

	h = make_hdr(127, 0, 0, 0, 0);
	CHECK(rtp_hdr_is_payload_type_valid(&h) == 1); /* dynamic range high end */

	h = make_hdr(128, 0, 0, 0, 0);
	CHECK(rtp_hdr_is_payload_type_valid(&h) == 0); /* just above dynamic range */

	h = make_hdr(33, 0, 0, 0, 0);
	CHECK(rtp_hdr_is_payload_type_valid(&h) == 1); /* MPEG-TS special case */

	h = make_hdr(0, 0, 0, 0, 0);
	CHECK(rtp_hdr_is_payload_type_valid(&h) == 0);
}

/* ---------------------------------------------------------------------
 * rtp_hdr_is_continious()
 * --------------------------------------------------------------------- */

static void test_is_continious_basic(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	/* First ever packet: no prior state, always reported continuous. */
	struct rtp_hdr h = make_hdr(96, 100, 0, 0, 0);
	CHECK(rtp_hdr_is_continious(&ctx, &h) == 1);

	/* seq+1: continuous. */
	h = make_hdr(96, 101, 0, 0, 0);
	CHECK(rtp_hdr_is_continious(&ctx, &h) == 1);

	/* skip a sequence number: discontinuous. */
	h = make_hdr(96, 105, 0, 0, 0);
	CHECK(rtp_hdr_is_continious(&ctx, &h) == 0);

	/* wraparound 65535 -> 0 is a continuous step. */
	h = make_hdr(96, 65535, 0, 0, 0);
	rtp_hdr_is_continious(&ctx, &h);
	h = make_hdr(96, 0, 0, 0, 0);
	CHECK(rtp_hdr_is_continious(&ctx, &h) == 1);

	rtp_analyzer_free(&ctx);
}

static void test_is_continious_seq_zero_sentinel_swallows_next_check(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	struct rtp_hdr h = make_hdr(96, 0, 0, 0, 0);
	rtp_hdr_is_continious(&ctx, &h); /* ctx->last.seq becomes 0 */

	h = make_hdr(96, 500, 0, 0, 0); /* wildly discontinuous */
	CHECK(rtp_hdr_is_continious(&ctx, &h) == 1); /* pinned bug: falsely reported continuous */

	rtp_analyzer_free(&ctx);
}

/* ---------------------------------------------------------------------
 * rtp_hdr_write() -- counters
 * --------------------------------------------------------------------- */

static void test_write_counts_total_packets_and_frames(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	struct rtp_hdr h = make_hdr(96, 1, 1000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.totalPackets, 1);
	CHECK_EQ_I64(ctx.totalFrames, 0); /* m == 0 */

	h = make_hdr(96, 2, 1000, 0xAA, 1);
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.totalPackets, 2);
	CHECK_EQ_I64(ctx.totalFrames, 1); /* m == 1 */

	rtp_analyzer_free(&ctx);
}

static void test_write_discontinuity_events(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	struct rtp_hdr h = make_hdr(96, 1, 1000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.discontinuityEvents, 0);

	h = make_hdr(96, 10, 1090, 0xAA, 0); /* sequence jumped */
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.discontinuityEvents, 1);

	rtp_analyzer_free(&ctx);
}

static void test_write_illegal_payload_type_events(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	struct rtp_hdr h = make_hdr(96, 1, 1000, 0xAA, 0); /* valid */
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegalPayloadTypeEvents, 0);

	h = make_hdr(50, 2, 1000, 0xAA, 0); /* invalid, outside dynamic range and not 33 */
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegalPayloadTypeEvents, 1);

	rtp_analyzer_free(&ctx);
}

/* ticks = ltntstools_pts_diff(old_ts, new_ts) / 90 (90kHz clock -> ms).
 * ticks == 0 -> stall; ticks in [1,2] -> unremarkable; ticks >= 3 -> movement. */
static void test_write_ts_timestamp_stall_and_movement(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	struct rtp_hdr h = make_hdr(96, 1, 1000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h); /* first packet: ctx->last.ts was 0, no comparison yet */

	h = make_hdr(96, 2, 1000, 0xAA, 0); /* same ts: diff 0 -> stall */
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegalTSTimestampStallEvents, 1);
	CHECK_EQ_I64(ctx.illegalTSTimestampMovementEvents, 0);

	h = make_hdr(96, 3, 1000 + 180, 0xAA, 0); /* diff 180 -> ticks 2: unremarkable */
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegalTSTimestampStallEvents, 1);
	CHECK_EQ_I64(ctx.illegalTSTimestampMovementEvents, 0);

	h = make_hdr(96, 4, 1000 + 180 + 270, 0xAA, 0); /* diff 270 -> ticks 3: movement */
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegalTSTimestampMovementEvents, 1);

	rtp_analyzer_free(&ctx);
}

static void test_write_2110_timestamp_movement(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	/* Previous packet has m == 0 (not end of frame) and pt in the 2110
	 * dynamic range; the timestamp then changes before the frame ended --
	 * illegal per SMPTE 2110 packetization rules. */
	struct rtp_hdr h = make_hdr(96, 1, 1000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);

	h = make_hdr(96, 2, 2000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegal2110TimestampMovementEvents, 1);

	rtp_analyzer_free(&ctx);
}

static void test_write_2110_timestamp_movement_not_counted_outside_dynamic_range(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	/* Same "m==0 then ts changes" shape, but pt == 33 (MPEG-TS, not 2110) --
	 * the movement counter only applies to pt 96..127. */
	struct rtp_hdr h = make_hdr(33, 1, 1000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);

	h = make_hdr(33, 2, 2000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegal2110TimestampMovementEvents, 0);

	rtp_analyzer_free(&ctx);
}

static void test_write_2110_timestamp_stall(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	/* Previous packet has m == 1 (end of frame); the next frame's first
	 * packet keeping the SAME timestamp is a stall. No pt filter on this path. */
	struct rtp_hdr h = make_hdr(96, 1, 1000, 0xAA, 1);
	rtp_hdr_write(&ctx, &h);

	h = make_hdr(96, 2, 1000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);
	CHECK_EQ_I64(ctx.illegal2110TimestampStallEvents, 1);

	rtp_analyzer_free(&ctx);
}

static void test_write_illegal_ts_counter_movement_never_fires(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	struct rtp_hdr h = make_hdr(96, 10, 1000, 0xAA, 0);
	rtp_hdr_write(&ctx, &h);

	h = make_hdr(96, 5, 2000, 0xAA, 0); /* genuine backward sequence jump */
	rtp_hdr_write(&ctx, &h);

	CHECK_EQ_I64(ctx.illegalTSCounterMovementEvents, 0); /* pinned bug, see file header */

	rtp_analyzer_free(&ctx);
}

static void test_analyzer_reset_clears_everything(void)
{
	struct rtp_hdr_analyzer_s ctx;
	rtp_analyzer_init(&ctx);

	struct rtp_hdr h = make_hdr(96, 1, 1000, 0xAA, 1);
	rtp_hdr_write(&ctx, &h);
	h = make_hdr(50, 10, 2000, 0xAA, 0); /* discontinuity + illegal pt */
	rtp_hdr_write(&ctx, &h);
	CHECK(ctx.totalPackets > 0);

	rtp_analyzer_reset(&ctx);

	CHECK_EQ_I64(ctx.totalPackets, 0);
	CHECK_EQ_I64(ctx.totalFrames, 0);
	CHECK_EQ_I64(ctx.discontinuityEvents, 0);
	CHECK_EQ_I64(ctx.illegalPayloadTypeEvents, 0);
	CHECK_EQ_I64(ctx.illegal2110TimestampMovementEvents, 0);
	CHECK_EQ_I64(ctx.illegal2110TimestampStallEvents, 0);
	CHECK_EQ_I64(ctx.illegalTSTimestampMovementEvents, 0);
	CHECK_EQ_I64(ctx.illegalTSTimestampStallEvents, 0);
	CHECK_EQ_I64(ctx.illegalTSCounterMovementEvents, 0);

	rtp_analyzer_free(&ctx);
}

/* ---------------------------------------------------------------------
 * rtp_frame_queryPositions()
 * --------------------------------------------------------------------- */

#define TS_PACKET_LEN 188
#define TS_PACKETS_PER_FRAME 7
#define RTP_FRAME_LEN (12 + (TS_PACKETS_PER_FRAME * TS_PACKET_LEN)) /* 1328 */

/* Writes one RTP-wrapped-MPEGTS "frame" (a 12 byte RTP header followed by
 * TS_PACKETS_PER_FRAME 188-byte TS packets, each starting with the 0x47
 * sync byte) at dst. Goes through a real `struct rtp_hdr *` so the wire
 * layout matches whatever this compiler's bitfield packing actually is --
 * the same layout rtp_frame_queryPositions() itself reinterprets. */
static void write_rtp_ts_frame(uint8_t *dst, uint8_t pt, uint32_t ssrc, uint16_t seq, int x, int cc)
{
	memset(dst, 0, RTP_FRAME_LEN);

	struct rtp_hdr *h = (struct rtp_hdr *)dst;
	h->version = 2;
	h->pt = pt;
	h->x = x ? 1 : 0;
	h->cc = cc & 0x0f;
	h->seq = htons(seq);
	h->ssrc = htonl(ssrc);

	for (int i = 0; i < TS_PACKETS_PER_FRAME; i++) {
		dst[12 + (i * TS_PACKET_LEN)] = 0x47;
	}
}

static void test_frame_queryPositions_finds_frames_with_lengths_and_offsets(void)
{
	const uint32_t ssrc = 0xAABBCCDD;
	const uint64_t addr = 1000;
	const int frameCount = 3;

	uint8_t *buf = malloc(RTP_FRAME_LEN * frameCount);
	for (int i = 0; i < frameCount; i++) {
		write_rtp_ts_frame(buf + (i * RTP_FRAME_LEN), 33, ssrc, 100 + i, 0, 0);
	}

	struct rtp_frame_position_s *arr = NULL;
	int arrLen = 0;
	int ret = rtp_frame_queryPositions(buf, RTP_FRAME_LEN * frameCount, addr, htonl(ssrc), &arr, &arrLen);

	CHECK(ret == 0);
	CHECK(arrLen == frameCount);
	if (arrLen == frameCount) {
		for (int i = 0; i < frameCount; i++) {
			CHECK_EQ_I64(arr[i].offset, addr + (i * RTP_FRAME_LEN));
		}
		/* Every frame except the last has a known length (distance to the
		 * next detected header); the last is documented to be left at 0. */
		CHECK_EQ_I64(arr[0].lengthBytes, RTP_FRAME_LEN);
		CHECK_EQ_I64(arr[1].lengthBytes, RTP_FRAME_LEN);
		CHECK_EQ_I64(arr[2].lengthBytes, 0);
	}

	free(arr);
	free(buf);
}

static void test_frame_queryPositions_filters_wrong_ssrc(void)
{
	const uint32_t wantSsrc = 0x11111111;
	const int frameCount = 2;

	uint8_t *buf = malloc(RTP_FRAME_LEN * frameCount);
	write_rtp_ts_frame(buf, 33, wantSsrc, 1, 0, 0);
	write_rtp_ts_frame(buf + RTP_FRAME_LEN, 33, 0x22222222 /* different ssrc */, 2, 0, 0);

	struct rtp_frame_position_s *arr = NULL;
	int arrLen = 0;
	CHECK(rtp_frame_queryPositions(buf, RTP_FRAME_LEN * frameCount, 0, htonl(wantSsrc), &arr, &arrLen) == 0);
	CHECK(arrLen == 1); /* only the matching-ssrc frame is found */

	free(arr);
	free(buf);
}

static void test_frame_queryPositions_filters_non_mpegts_payload_type(void)
{
	const uint32_t ssrc = 0x33333333;

	uint8_t *buf = malloc(RTP_FRAME_LEN);
	write_rtp_ts_frame(buf, 96 /* not 33 (MPEG-TS) */, ssrc, 1, 0, 0);

	struct rtp_frame_position_s *arr = NULL;
	int arrLen = 0;
	CHECK(rtp_frame_queryPositions(buf, RTP_FRAME_LEN, 0, htonl(ssrc), &arr, &arrLen) == 0);
	CHECK(arrLen == 0);

	free(arr);
	free(buf);
}

static void test_frame_queryPositions_filters_extension_and_csrc(void)
{
	const uint32_t ssrc = 0x44444444;

	uint8_t *buf = malloc(RTP_FRAME_LEN);
	write_rtp_ts_frame(buf, 33, ssrc, 1, 1 /* x=1 */, 0);
	struct rtp_frame_position_s *arr = NULL;
	int arrLen = 0;
	CHECK(rtp_frame_queryPositions(buf, RTP_FRAME_LEN, 0, htonl(ssrc), &arr, &arrLen) == 0);
	CHECK(arrLen == 0); /* extension bit set: discarded */
	free(arr);

	write_rtp_ts_frame(buf, 33, ssrc, 1, 0, 2 /* cc=2 */);
	arr = NULL; arrLen = 0;
	CHECK(rtp_frame_queryPositions(buf, RTP_FRAME_LEN, 0, htonl(ssrc), &arr, &arrLen) == 0);
	CHECK(arrLen == 0); /* multiple CSRC identifiers: discarded */
	free(arr);

	free(buf);
}

int main(void)
{
	test_payload_type_valid();

	test_is_continious_basic();
	test_is_continious_seq_zero_sentinel_swallows_next_check();

	test_write_counts_total_packets_and_frames();
	test_write_discontinuity_events();
	test_write_illegal_payload_type_events();
	test_write_ts_timestamp_stall_and_movement();
	test_write_2110_timestamp_movement();
	test_write_2110_timestamp_movement_not_counted_outside_dynamic_range();
	test_write_2110_timestamp_stall();
	test_write_illegal_ts_counter_movement_never_fires();
	test_analyzer_reset_clears_everything();

	test_frame_queryPositions_finds_frames_with_lengths_and_offsets();
	test_frame_queryPositions_filters_wrong_ssrc();
	test_frame_queryPositions_filters_non_mpegts_payload_type();
	test_frame_queryPositions_filters_extension_and_csrc();

	if (g_failures == 0) {
		printf("PASS: all rtp-analyzer tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d rtp-analyzer test(s) failed\n", g_failures);
	return 1;
}
