/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/libltntstools/klbitstream_readwriter.h. This is the
 * bit-level primitive underlying pes.c, pat.c and others (already
 * exercised indirectly by test_pes.c/test_pat.c), but had no tests
 * directly exercising the header's own API/edge cases -- which is exactly
 * where the bugs below were hiding. Header-only (all inline/static
 * functions), so this test file needs no other .c files and links against
 * nothing beyond the standard library.
 *
 * THREE REAL bugs were found while writing these tests and have been
 * FIXED in src/libltntstools/klbitstream_readwriter.h:
 *
 * 1. (Critical, confirmed with AddressSanitizer) klbs_write_bit(),
 *    klbs_read_bit() and klbs_read_byte_aligned() detect a buffer overrun
 *    (buflen_used >= buflen) and correctly set ctx->overrun = 1 -- but
 *    with this header's own KLBITSTREAM_RETURN_ON_OVERRUN compiled to 0
 *    (its default value, used everywhere in this codebase), the early
 *    `return` guarding the actual `*(ctx->buf + ctx->buflen_used++)`
 *    memory access is compiled out. The functions set the flag and then
 *    perform the out-of-bounds access anyway. Confirmed via repro:
 *    writing 24 bits into a 1-byte buffer (or reading 24 bits from one)
 *    produced an ASan stack-buffer-overflow. Fixed by guarding each of
 *    the three raw `ctx->buf` accesses with an explicit
 *    `buflen_used < buflen` check, independent of
 *    KLBITSTREAM_RETURN_ON_OVERRUN -- this changes no other control flow,
 *    return values, or overrun/truncated flagging; it only stops the
 *    handful of accesses that were touching memory beyond the buffer.
 *    test_write_bit_overrun_does_not_corrupt_beyond_buffer() and
 *    test_read_bit_overrun_does_not_read_beyond_buffer() are the
 *    regression tests (this file's whole suite also runs clean under
 *    AddressSanitizer, verified separately from the normal build).
 *
 * 2. klbs_write_buffer_complete() -- meant to flush/pad any partial
 *    trailing byte and leave the stream fully flushed -- looped
 *    `for (int i = ctx->reg_used; i <= 8; i++)`, one iteration too many:
 *    klbs_write_bit() flushes the byte and resets reg_used to 0 once it
 *    fills, so that extra iteration silently started a *new*,
 *    never-flushed one-bit partial byte. Bits written after a call to
 *    this function landed shifted by that stray bit in the next flushed
 *    byte -- directly contradicting the function's documented purpose
 *    ("ensures that any dangling trailing bits are properly stuffed").
 *    Fixed by changing the loop bound to `i < 8`.
 *    test_write_buffer_complete_leaves_stream_cleanly_aligned() is the
 *    regression test.
 *
 * 3. klbs_peek_bits() compared `ctx->buflen_used + bitcount >= ctx->buflen`
 *    to decide overrun -- but buflen/buflen_used are BYTE counts and
 *    bitcount is a BIT count, an 8x unit mismatch, and the check also
 *    ignored reg_used (bits already buffered and available without
 *    consuming another byte). This over-flagged overrun by roughly 8x:
 *    peeking a single byte (bitcount=8) out of a fresh, otherwise-roomy
 *    4-byte buffer already tripped it (confirmed via repro: overrun was
 *    set to 1 even though the correct byte value was still returned).
 *    Fixed by comparing bitcount against the actual remaining bit
 *    capacity: (buflen - buflen_used) * 8 + reg_used.
 *    test_peek_bits_does_not_false_positive_on_small_buffers() is the
 *    regression test.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "libltntstools/klbitstream_readwriter.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- alloc / free / init lifecycle -------- */

static void test_alloc_free_basic(void)
{
	struct klbs_context_s *ctx = klbs_alloc();
	CHECK(ctx != NULL);
	klbs_free(ctx);
}

static void test_alloc_init_with_storage_write_mode(void)
{
	struct klbs_context_s *ctx = klbs_alloc_init_with_storage(16, 1 /* write */);
	CHECK(ctx != NULL);
	CHECK(ctx->didAllocateStorage == 1);
	CHECK(klbs_get_buffer(ctx) != NULL);
	CHECK(klbs_get_buffer_size(ctx) == 16);
	CHECK(klbs_get_byte_count(ctx) == 0);

	klbs_write_bits(ctx, 0xFF, 8);
	CHECK(klbs_get_byte_count(ctx) == 1);

	free(klbs_get_buffer(ctx)); /* per docs: klbs_free() leaves the user buffer intact */
	klbs_free(ctx);
}

static void test_alloc_init_with_storage_read_mode(void)
{
	struct klbs_context_s *ctx = klbs_alloc_init_with_storage(4, 0 /* read */);
	CHECK(ctx != NULL);
	CHECK(klbs_get_buffer_size(ctx) == 4);

	free(klbs_get_buffer(ctx));
	klbs_free(ctx);
}

static void test_init_zeroes_context(void)
{
	uint8_t buf[8];
	struct klbs_context_s ctx;
	memset(&ctx, 0xAA, sizeof(ctx));

	klbs_init(&ctx);

	CHECK(ctx.buf == NULL);
	CHECK(ctx.buflen == 0);
	CHECK(ctx.buflen_used == 0);
	CHECK(ctx.reg_used == 0);
	CHECK(ctx.overrun == 0);
	CHECK(ctx.truncated == 0);

	klbs_write_set_buffer(&ctx, buf, sizeof(buf));
	CHECK(klbs_get_buffer(&ctx) == buf);
	CHECK(klbs_get_buffer_size(&ctx) == sizeof(buf));
}

/* -------- macros -------- */

static void test_byte_count_macros(void)
{
	uint8_t buf[10];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	CHECK(klbs_get_byte_count(&ctx) == 0);
	CHECK(klbs_get_byte_count_free(&ctx) == 10);

	klbs_write_bits(&ctx, 0x1234, 16); /* 2 bytes */
	CHECK(klbs_get_byte_count(&ctx) == 2);
	CHECK(klbs_get_byte_count_free(&ctx) == 8);
}

/* -------- write bit packing -------- */

static void test_write_bit_msb_first_packing(void)
{
	uint8_t buf[1];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	/* Bits 1,0,1,1,0,0,1,0 written one at a time -> 0b10110010. The
	 * first bit written ends up as the MSB. */
	int bits[8] = { 1, 0, 1, 1, 0, 0, 1, 0 };
	for (int i = 0; i < 8; i++)
		klbs_write_bit(&ctx, bits[i]);

	CHECK(ctx.overrun == 0);
	CHECK(klbs_get_byte_count(&ctx) == 1);
	CHECK(buf[0] == 0xB2);
}

static void test_write_bits_multi_byte_value(void)
{
	uint8_t buf[3];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	klbs_write_bits(&ctx, 0xAABBCC, 24);

	CHECK(ctx.overrun == 0);
	CHECK(klbs_get_byte_count(&ctx) == 3);
	CHECK(buf[0] == 0xAA);
	CHECK(buf[1] == 0xBB);
	CHECK(buf[2] == 0xCC);
}

/* -------- write/read round trips -------- */

static void roundtrip_one(uint64_t value, uint32_t bitcount)
{
	uint8_t buf[16];
	struct klbs_context_s w;
	klbs_init(&w);
	klbs_write_set_buffer(&w, buf, sizeof(buf));
	klbs_write_bits(&w, value, bitcount);
	klbs_write_buffer_complete(&w);
	CHECK(w.overrun == 0);

	struct klbs_context_s r;
	klbs_init(&r);
	klbs_read_set_buffer(&r, buf, sizeof(buf));
	uint64_t readback = klbs_read_bits(&r, bitcount);
	CHECK(r.overrun == 0);

	uint64_t mask = (bitcount >= 64) ? ~0ULL : ((1ULL << bitcount) - 1);
	CHECK((readback & mask) == (value & mask));
}

static void test_write_read_roundtrip_various_bitcounts(void)
{
	roundtrip_one(1, 1);
	roundtrip_one(0, 1);
	roundtrip_one(0xF, 4);
	roundtrip_one(0xABC, 12);
	roundtrip_one(0x1FFFFFFFF, 33);
	roundtrip_one(0xFFFFFFFFFFFFFFFFULL, 64);
	roundtrip_one(0x0123456789ABCDEFULL, 64);
}

/* -------- byte stuffing -------- */

static void test_write_byte_stuff_pads_partial_register(void)
{
	uint8_t buf[1];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	klbs_write_bits(&ctx, 0b101, 3); /* 3 bits pending, not yet flushed */
	CHECK(klbs_get_byte_count(&ctx) == 0);

	klbs_write_byte_stuff(&ctx, 0); /* pad remaining 5 bits with 0 */

	CHECK(ctx.overrun == 0);
	CHECK(klbs_get_byte_count(&ctx) == 1);
	CHECK(buf[0] == 0xA0); /* 101 00000 */
}

static void test_write_byte_stuff_with_one_bits(void)
{
	uint8_t buf[1];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	klbs_write_bits(&ctx, 0b11, 2);
	klbs_write_byte_stuff(&ctx, 1); /* pad remaining 6 bits with 1 */

	CHECK(klbs_get_byte_count(&ctx) == 1);
	CHECK(buf[0] == 0xFF); /* 11 111111 */
}

static void test_write_byte_stuff_noop_when_already_aligned(void)
{
	uint8_t buf[2];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	klbs_write_bits(&ctx, 0xAB, 8); /* already byte-aligned */
	CHECK(klbs_get_byte_count(&ctx) == 1);

	klbs_write_byte_stuff(&ctx, 1); /* nothing pending -- must be a no-op */
	CHECK(klbs_get_byte_count(&ctx) == 1);
}

static void test_read_byte_stuff_discards_partial_register(void)
{
	uint8_t buf[1] = { 0xFF };
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_read_set_buffer(&ctx, buf, sizeof(buf));

	klbs_read_bits(&ctx, 3); /* pulls the whole byte into the register, 3 bits consumed */
	CHECK(ctx.reg_used == 5);

	klbs_read_byte_stuff(&ctx);
	CHECK(ctx.reg_used == 0);
	CHECK(ctx.overrun == 0);
}

/* -------- regression: write_buffer_complete leaves a clean, flushed state -------- */

static void test_write_buffer_complete_leaves_stream_cleanly_aligned(void)
{
	uint8_t buf[8];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	klbs_write_bits(&ctx, 0b101, 3);
	klbs_write_buffer_complete(&ctx);

	CHECK(ctx.reg_used == 0); /* fully flushed, no dangling stray bit */
	CHECK(klbs_get_byte_count(&ctx) == 1);
	CHECK(buf[0] == 0xA0);

	/* A bit written after "complete" must start a fresh, cleanly-aligned
	 * byte, not get shifted in behind a leftover stray bit. */
	klbs_write_bits(&ctx, 0b1, 1);
	klbs_write_buffer_complete(&ctx);

	CHECK(ctx.reg_used == 0);
	CHECK(klbs_get_byte_count(&ctx) == 2);
	CHECK(buf[1] == 0x80);
}

/* -------- peek -------- */

static void test_peek_bits_does_not_advance_or_mutate_original(void)
{
	uint8_t buf[2] = { 0x12, 0x34 };
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_read_set_buffer(&ctx, buf, sizeof(buf));

	uint64_t first = klbs_peek_bits(&ctx, 8);
	uint64_t second = klbs_peek_bits(&ctx, 8);

	CHECK(first == 0x12);
	CHECK(second == 0x12); /* repeated peeks return the same content */
	CHECK(ctx.buflen_used == 0); /* original context untouched */
	CHECK(ctx.reg_used == 0);

	uint64_t actualRead = klbs_read_bits(&ctx, 8);
	CHECK(actualRead == 0x12); /* peek didn't consume anything */
}

/* Regression test for the fixed unit-mismatch bug: see file header. */
static void test_peek_bits_does_not_false_positive_on_small_buffers(void)
{
	uint8_t buf[4] = { 0x12, 0x34, 0x56, 0x78 };
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_read_set_buffer(&ctx, buf, sizeof(buf));

	/* A single-byte peek out of a small-but-otherwise-roomy 4-byte
	 * buffer must not be flagged as an overrun. */
	uint64_t v = klbs_peek_bits(&ctx, 8);
	CHECK(v == 0x12);
	CHECK(ctx.overrun == 0);

	/* Peeking exactly the remaining bit capacity (all 32 bits) must also
	 * succeed cleanly. */
	struct klbs_context_s ctx2;
	klbs_init(&ctx2);
	klbs_read_set_buffer(&ctx2, buf, sizeof(buf));
	uint64_t all = klbs_peek_bits(&ctx2, 32);
	CHECK(all == 0x12345678ULL);
	CHECK(ctx2.overrun == 0);

	/* Peeking one bit more than actually remains must correctly overrun. */
	struct klbs_context_s ctx3;
	klbs_init(&ctx3);
	klbs_read_set_buffer(&ctx3, buf, sizeof(buf));
	klbs_peek_bits(&ctx3, 33);
	CHECK(ctx3.overrun == 1);
}

/* -------- overrun: does not touch memory beyond the buffer -------- */

/* Regression test for the fixed critical bug: see file header. This test's
 * real value is running clean under AddressSanitizer (verified separately);
 * under a plain build it documents the same state-level contract: overrun
 * gets flagged and buflen_used never exceeds buflen. */
static void test_write_bit_overrun_does_not_corrupt_beyond_buffer(void)
{
	uint8_t buf[1];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));

	klbs_write_bits(&ctx, 0xAABBCC, 24); /* 3 bytes into a 1-byte buffer */

	CHECK(ctx.overrun == 1);
	CHECK(ctx.buflen_used <= ctx.buflen);
	CHECK(buf[0] == 0xAA); /* the one byte that legitimately fit */
}

static void test_read_bit_overrun_does_not_read_beyond_buffer(void)
{
	uint8_t buf[1] = { 0xAB };
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_read_set_buffer(&ctx, buf, sizeof(buf));

	uint64_t v = klbs_read_bits(&ctx, 24); /* 3 bytes from a 1-byte buffer */

	CHECK(ctx.overrun == 1);
	CHECK(ctx.buflen_used <= ctx.buflen);
	CHECK(v == 0xAB0000ULL); /* real byte, zero-padded for the missing 2 */
}

/* -------- bitmove / bitcopy -------- */

static void test_bitmove_transfers_and_advances_src(void)
{
	uint8_t srcBuf[2] = { 0xAB, 0xCD };
	struct klbs_context_s src;
	klbs_init(&src);
	klbs_read_set_buffer(&src, srcBuf, sizeof(srcBuf));

	uint8_t dstBuf[2];
	struct klbs_context_s dst;
	klbs_init(&dst);
	klbs_write_set_buffer(&dst, dstBuf, sizeof(dstBuf));

	klbs_bitmove(&dst, &src, 16);

	CHECK(src.overrun == 0);
	CHECK(dst.overrun == 0);
	CHECK(klbs_get_byte_count(&src) == 2); /* src was consumed */
	CHECK(klbs_get_byte_count(&dst) == 2);
	CHECK(dstBuf[0] == 0xAB);
	CHECK(dstBuf[1] == 0xCD);
}

static void test_bitcopy_transfers_without_advancing_src(void)
{
	uint8_t srcBuf[2] = { 0xAB, 0xCD };
	struct klbs_context_s src;
	klbs_init(&src);
	klbs_read_set_buffer(&src, srcBuf, sizeof(srcBuf));

	uint8_t dstBuf[2];
	struct klbs_context_s dst;
	klbs_init(&dst);
	klbs_write_set_buffer(&dst, dstBuf, sizeof(dstBuf));

	klbs_bitcopy(&dst, &src, 16);

	CHECK(src.overrun == 0);
	CHECK(dst.overrun == 0);
	CHECK(klbs_get_byte_count(&src) == 0); /* src untouched */
	CHECK(klbs_get_byte_count(&dst) == 2);
	CHECK(dstBuf[0] == 0xAB);
	CHECK(dstBuf[1] == 0xCD);
}

static void test_bitcopy_overrun_when_insufficient_room(void)
{
	uint8_t srcBuf[1] = { 0xFF };
	struct klbs_context_s src;
	klbs_init(&src);
	klbs_read_set_buffer(&src, srcBuf, sizeof(srcBuf));

	uint8_t dstBuf[4];
	struct klbs_context_s dst;
	klbs_init(&dst);
	klbs_write_set_buffer(&dst, dstBuf, sizeof(dstBuf));

	klbs_bitcopy(&dst, &src, 16); /* only 8 bits available in src */

	CHECK(src.overrun == 1);
	CHECK(dst.overrun == 1);
}

/* -------- save -------- */

static void test_save_writes_used_bytes_to_file(void)
{
	uint8_t buf[4];
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_write_set_buffer(&ctx, buf, sizeof(buf));
	klbs_write_bits(&ctx, 0xAABB, 16); /* only 2 of 4 bytes used */

	char path[] = "/tmp/klbs_save_test_XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd >= 0)
		close(fd);

	CHECK(klbs_save(&ctx, path) == 0);

	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f) {
		uint8_t readback[8] = { 0 };
		size_t n = fread(readback, 1, sizeof(readback), f);
		CHECK(n == 2); /* only buflen_used bytes saved, not the full buffer */
		CHECK(readback[0] == 0xAA);
		CHECK(readback[1] == 0xBB);
		fclose(f);
	}
	unlink(path);
}

/* -------- peek_print_binary: smoke test (console output, no assertions) -------- */

static void test_peek_print_binary_does_not_crash(void)
{
	uint8_t buf[2] = { 0xAB, 0xCD };
	struct klbs_context_s ctx;
	klbs_init(&ctx);
	klbs_read_set_buffer(&ctx, buf, sizeof(buf));

	klbs_peek_print_binary(&ctx, 16);
	CHECK(ctx.buflen_used == 0); /* peek variant, must not consume */
}

int main(void)
{
	test_alloc_free_basic();
	test_alloc_init_with_storage_write_mode();
	test_alloc_init_with_storage_read_mode();
	test_init_zeroes_context();

	test_byte_count_macros();

	test_write_bit_msb_first_packing();
	test_write_bits_multi_byte_value();
	test_write_read_roundtrip_various_bitcounts();

	test_write_byte_stuff_pads_partial_register();
	test_write_byte_stuff_with_one_bits();
	test_write_byte_stuff_noop_when_already_aligned();
	test_read_byte_stuff_discards_partial_register();

	test_write_buffer_complete_leaves_stream_cleanly_aligned();

	test_peek_bits_does_not_advance_or_mutate_original();
	test_peek_bits_does_not_false_positive_on_small_buffers();

	test_write_bit_overrun_does_not_corrupt_beyond_buffer();
	test_read_bit_overrun_does_not_read_beyond_buffer();

	test_bitmove_transfers_and_advances_src();
	test_bitcopy_transfers_without_advancing_src();
	test_bitcopy_overrun_when_insufficient_room();

	test_save_writes_used_bytes_to_file();

	test_peek_print_binary_does_not_crash();

	if (g_failures == 0) {
		printf("PASS: all klbitstream_readwriter tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d klbitstream_readwriter test(s) failed\n", g_failures);
	return 1;
}
