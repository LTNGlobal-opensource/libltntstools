/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for the ltntstools_reframer framework
 * (src/reframer.c / src/libltntstools/reframer.h).
 * Builds directly against ../src/reframer.c, no other library dependencies.
 *
 * NOTE: the public write function is named ltststools_reframer_write()
 * (missing the 'n') -- that's the real, existing exported symbol name in
 * reframer.h, not a typo introduced here.
 *
 * NOTE: this module has no flush/drain API. Bytes that don't complete a
 * full frame stay buffered in ctx->sendBuffer indefinitely and are silently
 * discarded by ltntstools_reframer_free(). That's exercised below
 * (test_free_after_partial_data_no_crash) as documented behaviour, not a bug.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/reframer.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* Captures every callback invocation: the concatenation of all emitted
 * bytes (in order), plus per-frame lengths, so tests can assert both
 * framing (sizes/count) and content/ordering correctness. */
struct capture_s {
	uint8_t *data;
	int len;
	int frameCount;
	int frameLens[256];
};

static void capture_reset(struct capture_s *c)
{
	free(c->data);
	memset(c, 0, sizeof(*c));
}

static void capture_cb(void *userContext, const uint8_t *buf, int lengthBytes)
{
	struct capture_s *c = (struct capture_s *)userContext;

	c->data = realloc(c->data, c->len + lengthBytes);
	memcpy(c->data + c->len, buf, lengthBytes);
	c->len += lengthBytes;

	if (c->frameCount < (int)(sizeof(c->frameLens) / sizeof(c->frameLens[0]))) {
		c->frameLens[c->frameCount] = lengthBytes;
	}
	c->frameCount++;
}

static void fill_sequential(uint8_t *buf, int len, int start)
{
	for (int i = 0; i < len; i++) {
		buf[i] = (uint8_t)(start + i);
	}
}

/* -------- alloc / free -------- */

static void test_alloc_returns_null_for_null_callback(void)
{
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(NULL, 1316, NULL);
	CHECK(ctx == NULL);
}

static void test_alloc_basic(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 1316, capture_cb);
	CHECK(ctx != NULL);
	ltntstools_reframer_free(ctx);
}

static void test_write_null_ctx_returns_error(void)
{
	uint8_t buf[4] = { 0 };
	CHECK(ltststools_reframer_write(NULL, buf, sizeof(buf)) < 0);
}

/* -------- frameSizeBytes == 0: passthrough mode -------- */

static void test_passthrough_mode(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 0, capture_cb);
	CHECK(ctx != NULL);

	uint8_t buf[13];
	fill_sequential(buf, sizeof(buf), 0);

	CHECK(ltststools_reframer_write(ctx, buf, sizeof(buf)) == 0);
	CHECK(cap.frameCount == 1);
	CHECK(cap.frameLens[0] == 13);
	CHECK(cap.len == 13);
	CHECK(memcmp(cap.data, buf, 13) == 0);

	/* A second, differently-sized write must also pass straight through. */
	uint8_t buf2[3] = { 0xAA, 0xBB, 0xCC };
	CHECK(ltststools_reframer_write(ctx, buf2, sizeof(buf2)) == 0);
	CHECK(cap.frameCount == 2);
	CHECK(cap.frameLens[1] == 3);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

/* -------- basic buffered framing -------- */

static void test_write_empty_buffer_no_callback(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	CHECK(ltststools_reframer_write(ctx, (const uint8_t *)"", 0) == 0);
	CHECK(cap.frameCount == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

static void test_single_write_exact_frame_size(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	uint8_t buf[7];
	fill_sequential(buf, sizeof(buf), 0);

	CHECK(ltststools_reframer_write(ctx, buf, sizeof(buf)) == 0);
	CHECK(cap.frameCount == 1);
	CHECK(cap.frameLens[0] == 7);
	CHECK(memcmp(cap.data, buf, 7) == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

static void test_single_write_multiple_frames(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	uint8_t buf[21]; /* exactly 3 frames of 7 */
	fill_sequential(buf, sizeof(buf), 0);

	CHECK(ltststools_reframer_write(ctx, buf, sizeof(buf)) == 0);
	CHECK(cap.frameCount == 3);
	CHECK(cap.frameLens[0] == 7 && cap.frameLens[1] == 7 && cap.frameLens[2] == 7);
	CHECK(cap.len == 21);
	CHECK(memcmp(cap.data, buf, 21) == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

static void test_single_write_frame_plus_remainder(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	uint8_t buf[10]; /* one full frame (7) + 3 leftover */
	fill_sequential(buf, sizeof(buf), 0);

	CHECK(ltststools_reframer_write(ctx, buf, sizeof(buf)) == 0);
	CHECK(cap.frameCount == 1);
	CHECK(cap.frameLens[0] == 7);
	CHECK(memcmp(cap.data, buf, 7) == 0);

	/* Complete the remainder with a follow-up write, verify the 2nd frame
	 * correctly stitches together the tail of the first write and the head
	 * of the second, in order. */
	uint8_t buf2[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
	CHECK(ltststools_reframer_write(ctx, buf2, sizeof(buf2)) == 0);
	CHECK(cap.frameCount == 2);
	CHECK(cap.frameLens[1] == 7);

	uint8_t expected_frame2[7] = { buf[7], buf[8], buf[9], 0xAA, 0xBB, 0xCC, 0xDD };
	CHECK(memcmp(cap.data + 7, expected_frame2, 7) == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

static void test_unflushed_remainder_not_emitted_without_completion(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	uint8_t buf[5]; /* less than one frame */
	fill_sequential(buf, sizeof(buf), 0);

	CHECK(ltststools_reframer_write(ctx, buf, sizeof(buf)) == 0);
	CHECK(cap.frameCount == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

/* -------- accumulation across many small, unaligned writes -------- */

static void test_accumulate_across_multiple_small_writes(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	uint8_t src[20];
	fill_sequential(src, sizeof(src), 0);

	/* Feed 4 bytes at a time: 5 writes of 4 bytes = 20 bytes total.
	 * floor(20/7) = 2 complete frames, 6 bytes left buffered. */
	for (int off = 0; off < 20; off += 4) {
		CHECK(ltststools_reframer_write(ctx, src + off, 4) == 0);
	}

	CHECK(cap.frameCount == 2);
	CHECK(cap.len == 14);
	CHECK(memcmp(cap.data, src, 14) == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

static void test_frame_completed_across_two_writes(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	uint8_t part1[3] = { 1, 2, 3 };
	uint8_t part2[4] = { 4, 5, 6, 7 };

	CHECK(ltststools_reframer_write(ctx, part1, sizeof(part1)) == 0);
	CHECK(cap.frameCount == 0); /* not enough yet */

	CHECK(ltststools_reframer_write(ctx, part2, sizeof(part2)) == 0);
	CHECK(cap.frameCount == 1);
	CHECK(cap.frameLens[0] == 7);

	uint8_t expected[7] = { 1, 2, 3, 4, 5, 6, 7 };
	CHECK(memcmp(cap.data, expected, 7) == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

/* -------- degenerate 1-byte frame size -------- */

static void test_frame_size_one_byte(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 1, capture_cb);

	uint8_t buf[5] = { 10, 20, 30, 40, 50 };
	CHECK(ltststools_reframer_write(ctx, buf, sizeof(buf)) == 0);

	CHECK(cap.frameCount == 5);
	for (int i = 0; i < 5; i++) {
		CHECK(cap.frameLens[i] == 1);
	}
	CHECK(memcmp(cap.data, buf, 5) == 0);

	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

/* -------- end-to-end order/content integrity over many irregular writes -------- */

static void test_content_order_preserved_over_many_writes(void)
{
	struct capture_s cap = { 0 };
	const int frameSize = 16;
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, frameSize, capture_cb);

	const int totalLen = 1000;
	uint8_t *src = malloc(totalLen);
	fill_sequential(src, totalLen, 0); /* wraps mod 256, still deterministic */

	/* Feed with a repeating, non-frame-aligned chunk size cycle. */
	const int chunkSizes[] = { 1, 2, 3, 5, 8 };
	int off = 0, chunkIdx = 0;
	while (off < totalLen) {
		int c = chunkSizes[chunkIdx % (sizeof(chunkSizes) / sizeof(chunkSizes[0]))];
		chunkIdx++;
		if (c > totalLen - off) {
			c = totalLen - off;
		}
		CHECK(ltststools_reframer_write(ctx, src + off, c) == 0);
		off += c;
	}

	int expectedFrames = totalLen / frameSize;
	int expectedBytes = expectedFrames * frameSize;

	CHECK(cap.frameCount == expectedFrames);
	CHECK(cap.len == expectedBytes);
	CHECK(memcmp(cap.data, src, expectedBytes) == 0);

	free(src);
	capture_reset(&cap);
	ltntstools_reframer_free(ctx);
}

/* -------- free() with unflushed partial data must not crash -------- */

static void test_free_after_partial_data_no_crash(void)
{
	struct capture_s cap = { 0 };
	struct ltntstools_reframer_ctx_s *ctx = ltntstools_reframer_alloc(&cap, 7, capture_cb);

	uint8_t buf[3] = { 1, 2, 3 };
	CHECK(ltststools_reframer_write(ctx, buf, sizeof(buf)) == 0);
	CHECK(cap.frameCount == 0); /* buffered, not yet emitted */

	ltntstools_reframer_free(ctx); /* must not crash; partial data is silently dropped */
	capture_reset(&cap);
}

int main(void)
{
	test_alloc_returns_null_for_null_callback();
	test_alloc_basic();
	test_write_null_ctx_returns_error();

	test_passthrough_mode();

	test_write_empty_buffer_no_callback();
	test_single_write_exact_frame_size();
	test_single_write_multiple_frames();
	test_single_write_frame_plus_remainder();
	test_unflushed_remainder_not_emitted_without_completion();

	test_accumulate_across_multiple_small_writes();
	test_frame_completed_across_two_writes();

	test_frame_size_one_byte();
	test_content_order_preserved_over_many_writes();

	test_free_after_partial_data_no_crash();

	if (g_failures == 0) {
		printf("PASS: all reframer tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d reframer test(s) failed\n", g_failures);
	return 1;
}
