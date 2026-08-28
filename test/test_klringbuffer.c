/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/klringbuffer.c / src/klringbuffer.h.
 * Builds against ../src/klringbuffer.c directly (self-contained). Requires
 * -lpthread for the (optional, opt-in via rb_new_threadsafe()) mutex.
 *
 * Three real bugs in the wraparound/overflow/discard logic were found and
 * fixed while writing this file (confirmed with AddressSanitizer before
 * the fix, and re-confirmed clean after):
 *  1. rb_write_with_state(): a single write larger than the ring's current
 *     physical allocation (after growth was refused because it would
 *     exceed size_max) memcpy'd the full oversized length starting at
 *     `tail`, running past the end of the heap allocation.
 *  2. rb_discard(): passed its `bytes` argument straight through to
 *     fill -= bytes with no clamp (unlike rb_read()/rb_peek(), which do
 *     clamp); discarding more than rb_used() underflowed the unsigned
 *     `fill` counter, permanently corrupting the ring's accounting.
 *  3. rb_write_with_state(): the single-segment-vs-wraparound branch was
 *     decided by comparing tail/write_end pointers. Because write_end is
 *     computed mod buf->size, a write whose length is an exact multiple of
 *     buf->size (most commonly: writing exactly the ring's full capacity
 *     in one call) makes write_end wrap back around to equal tail --
 *     indistinguishable from "0 bytes to write" -- so it took the
 *     single-segment path and overflowed the buffer. This one didn't need
 *     size_max/overflow involved at all, just an ordinary full-capacity
 *     write after the head had rotated away from 0.
 *  4. _rb_grow(): grew the buffer via realloc()-in-place, which preserves
 *     byte offsets but not the ring's logical order. If the ring was
 *     wrapped (content split across the physical end) at the moment
 *     growth triggered, the segment that had wrapped to the front of the
 *     buffer got stranded there -- silently corrupting/losing data (no
 *     crash, no overflow flag), since the new, larger modulo arithmetic
 *     never looks for it at that offset again. Found only after this file
 *     already existed, by deliberately combining two conditions ("ring is
 *     wrapped" and "next write forces growth") that had each individually
 *     been tested but never together.
 * test_write_overflow_truncates_to_trailing_bytes(), test_discard_more_than_used_clamps_to_zero(),
 * test_write_exact_capacity_after_rotation() and test_write_grows_correctly_while_wrapped()
 * are direct regression coverage for bugs 1, 2, 3 and 4 respectively.
 *
 * Three more bugs, each with its own test below:
 *  5. rb_new_threadsafe() dereferenced rb_new()'s result unconditionally,
 *     crashing instead of propagating the NULL rb_new() documents returning
 *     for a bad size relationship or an OOM.
 *  6. rb_free(NULL) crashed: the RB_LOCK(rb) macro dereferences rb (via
 *     rb->usingMutex) before the function's own NULL check ever ran.
 *  7. rb_read_alloc() dereferenced its `to` output parameter with no NULL
 *     check at all, and relied on rb_reader()'s assert-only (compiled out
 *     under NDEBUG) check for `buf`.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "klringbuffer.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- rb_new() / rb_new_threadsafe() -------- */

static void test_new_rejects_zero_size(void)
{
	CHECK(rb_new(0, 100) == NULL);
}

static void test_new_rejects_size_greater_than_max(void)
{
	CHECK(rb_new(100, 10) == NULL);
}

static void test_new_accepts_size_equal_to_max(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	CHECK(rb != NULL);
	rb_free(rb);
}

static void test_new_threadsafe_basic(void)
{
	KLRingBuffer *rb = rb_new_threadsafe(8, 64);
	CHECK(rb != NULL);
	CHECK(rb_is_empty(rb));

	int overflow = 0;
	rb_write_with_state(rb, "hi", 2, &overflow);
	CHECK(overflow == 0);
	CHECK(rb_used(rb) == 2);

	rb_free(rb);
}

/* rb_new_threadsafe() used to dereference rb_new()'s result unconditionally,
 * crashing instead of propagating the NULL that rb_new() itself documents
 * returning for a bad size relationship. */
static void test_new_threadsafe_rejects_zero_size(void)
{
	CHECK(rb_new_threadsafe(0, 100) == NULL);
}

/* rb_free(NULL) used to crash: the RB_LOCK(rb) macro dereferences rb before
 * the function's own NULL check ever ran. Reaching the CHECK below without
 * crashing is the point of this test. */
static void test_free_null_is_noop(void)
{
	rb_free(NULL);
	CHECK(1);
}

/* -------- rb_is_empty() / rb_is_full() / rb_used() / rb_unused() -------- */

static void test_is_empty(void)
{
	KLRingBuffer *rb = rb_new(4, 4);
	CHECK(rb_is_empty(rb) == true);

	int overflow = 0;
	rb_write_with_state(rb, "a", 1, &overflow);
	CHECK(rb_is_empty(rb) == false);

	char out[1];
	rb_read(rb, out, 1);
	CHECK(rb_is_empty(rb) == true);

	rb_free(rb);
}

static void test_is_full(void)
{
	/* No growth headroom at all: fill == size_max the moment it's full. */
	KLRingBuffer *rb = rb_new(1, 1);
	CHECK(rb_is_full(rb) == false);

	int overflow = 0;
	rb_write_with_state(rb, "a", 1, &overflow);
	CHECK(overflow == 0);
	CHECK(rb_is_full(rb) == true);

	rb_free(rb);
}

static void test_used_and_unused(void)
{
	KLRingBuffer *rb = rb_new(4, 10);
	CHECK(rb_used(rb) == 0);
	CHECK(rb_unused(rb) == 10);

	int overflow = 0;
	rb_write_with_state(rb, "abc", 3, &overflow);
	CHECK(rb_used(rb) == 3);
	CHECK(rb_unused(rb) == 7);

	rb_free(rb);
}

static void test_empty_resets_without_freeing(void)
{
	KLRingBuffer *rb = rb_new(4, 4);
	int overflow = 0;
	rb_write_with_state(rb, "abcd", 4, &overflow);
	CHECK(rb_used(rb) == 4);

	rb_empty(rb);
	CHECK(rb_used(rb) == 0);
	CHECK(rb_is_empty(rb));

	/* Buffer must still be usable after rb_empty(). */
	rb_write_with_state(rb, "xy", 2, &overflow);
	CHECK(rb_used(rb) == 2);
	char out[2] = { 0 };
	rb_read(rb, out, 2);
	CHECK(memcmp(out, "xy", 2) == 0);

	rb_free(rb);
}

/* -------- rb_write_with_state() / rb_write() / rb_read() / rb_peek() round-trips -------- */

static void test_write_read_roundtrip(void)
{
	KLRingBuffer *rb = rb_new(16, 16);
	int overflow = 0;

	size_t w = rb_write_with_state(rb, "hello", 5, &overflow);
	CHECK(w == 5);
	CHECK(overflow == 0);
	CHECK(rb_used(rb) == 5);

	char out[5] = { 0 };
	size_t r = rb_read(rb, out, 5);
	CHECK(r == 5);
	CHECK(memcmp(out, "hello", 5) == 0);
	CHECK(rb_used(rb) == 0);

	rb_free(rb);
}

static void test_deprecated_write_wrapper(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	size_t w = rb_write(rb, "abcd", 4);
	CHECK(w == 4);
	CHECK(rb_used(rb) == 4);
	rb_free(rb);
}

static void test_peek_does_not_drain(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	int overflow = 0;
	rb_write_with_state(rb, "abcd", 4, &overflow);

	char out[4] = { 0 };
	size_t p = rb_peek(rb, out, 4);
	CHECK(p == 4);
	CHECK(memcmp(out, "abcd", 4) == 0);
	CHECK(rb_used(rb) == 4); /* unchanged: peek must not drain */

	memset(out, 0, sizeof(out));
	size_t r = rb_read(rb, out, 4);
	CHECK(r == 4);
	CHECK(memcmp(out, "abcd", 4) == 0);
	CHECK(rb_used(rb) == 0);

	rb_free(rb);
}

static void test_read_more_than_available_clamps(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	int overflow = 0;
	rb_write_with_state(rb, "ab", 2, &overflow);

	char out[8] = { 0 };
	size_t r = rb_read(rb, out, 8); /* only 2 bytes actually available */
	CHECK(r == 2);
	CHECK(memcmp(out, "ab", 2) == 0);

	rb_free(rb);
}

static void test_read_from_empty_returns_zero(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	char out[4];
	CHECK(rb_read(rb, out, 4) == 0);
	rb_free(rb);
}

static void test_read_alloc(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	int overflow = 0;
	rb_write_with_state(rb, "wxyz", 4, &overflow);

	char *out = NULL;
	size_t r = rb_read_alloc(rb, &out, 4);
	CHECK(r == 4);
	CHECK(out != NULL);
	CHECK(memcmp(out, "wxyz", 4) == 0);
	CHECK(rb_used(rb) == 0);

	free(out);
	rb_free(rb);
}

/* rb_read_alloc() used to dereference its `to` output parameter (and
 * inherit rb_reader()'s assert-only, NDEBUG-compiled-out `buf` check) with
 * no guard at all. */
static void test_read_alloc_rejects_null_args(void)
{
	KLRingBuffer *rb = rb_new(8, 8);

	char *out = (char *)0x1; /* sentinel: must stay untouched */
	CHECK(rb_read_alloc(NULL, &out, 4) == 0);
	CHECK(out == (char *)0x1);

	CHECK(rb_read_alloc(rb, NULL, 4) == 0);

	rb_free(rb);
}

static void test_wraparound_write_and_read(void)
{
	/* Rotate the head away from 0 first, then write/read across the
	 * physical end of the buffer to exercise the two-segment copy path
	 * in both rb_write_with_state() and rb_reader(). */
	KLRingBuffer *rb = rb_new(8, 8);
	int overflow = 0;

	rb_write_with_state(rb, "123456", 6, &overflow); /* fill=6 */
	char sink[6];
	rb_read(rb, sink, 6); /* head advances to 6, fill=0 */

	size_t w = rb_write_with_state(rb, "ABCDEF", 6, &overflow); /* wraps past offset 8 */
	CHECK(w == 6);
	CHECK(overflow == 0);
	CHECK(rb_used(rb) == 6);

	char out[6] = { 0 };
	size_t r = rb_read(rb, out, 6);
	CHECK(r == 6);
	CHECK(memcmp(out, "ABCDEF", 6) == 0);

	rb_free(rb);
}

/* Regression test for bug #3: a write whose length is an exact multiple of
 * the ring's current size, issued after the head has rotated away from 0,
 * used to make the wraparound-detection pointer comparison ambiguous and
 * overflow the allocation. No growth or size_max limit is involved here at
 * all -- this is ordinary full-capacity usage. */
static void test_write_exact_capacity_after_rotation(void)
{
	KLRingBuffer *rb = rb_new(4, 4);
	int overflow = 0;

	rb_write_with_state(rb, "AA", 2, &overflow);
	char sink[2];
	rb_read(rb, sink, 2); /* fully drains: head becomes 2, fill becomes 0 */
	CHECK(rb_used(rb) == 0);

	size_t w = rb_write_with_state(rb, "BBBB", 4, &overflow); /* exactly buf->size, head != 0 */
	CHECK(w == 4);
	CHECK(overflow == 0);
	CHECK(rb_used(rb) == 4);

	char out[4] = { 0 };
	CHECK(rb_read(rb, out, 4) == 4);
	CHECK(memcmp(out, "BBBB", 4) == 0);

	rb_free(rb);
}

/* -------- growth -------- */

static void test_write_grows_buffer_within_max(void)
{
	/* Growth increments are bytes*128, so pick a small next write (1 byte
	 * -> +128) against a size_max with enough headroom (200) for that
	 * increment to actually fit, unlike the size_max==size overflow tests
	 * below where growth is never possible at all. */
	KLRingBuffer *rb = rb_new(4, 200);
	int overflow = 0;

	rb_write_with_state(rb, "1234", 4, &overflow); /* fills initial allocation exactly */
	CHECK(overflow == 0);

	size_t w = rb_write_with_state(rb, "5", 1, &overflow); /* forces growth, not discard */
	CHECK(w == 1);
	CHECK(overflow == 0); /* grew instead of discarding */
	CHECK(rb_used(rb) == 5);

	char out[5] = { 0 };
	CHECK(rb_read(rb, out, 5) == 5);
	CHECK(memcmp(out, "12345", 5) == 0);

	rb_free(rb);
}

/* Regression test for bug #4: growth used to realloc() in place, which
 * preserves byte offsets but not the ring's logical order. If the ring was
 * wrapped (content split across the physical end) at the moment growth was
 * triggered, the segment that had wrapped to the front of the buffer got
 * stranded there -- silently corrupting/losing data, since it's nowhere
 * near where the new (larger) modulo arithmetic looks for it. No crash, no
 * overflow flag, just wrong bytes read back -- confirmed with a standalone
 * repro before the fix (_rb_grow() now linearizes into a fresh allocation
 * instead of realloc()-in-place). */
static void test_write_grows_correctly_while_wrapped(void)
{
	KLRingBuffer *rb = rb_new(4, 200);
	int overflow = 0;

	rb_write_with_state(rb, "AB", 2, &overflow);
	char sink[2];
	rb_read(rb, sink, 2); /* head=2, fill=0 */

	/* "CDEF" wraps: to_end from head=2 in a 4-byte ring is 2, so "CD"
	 * lands at offset 2-3 and "EF" wraps around to offset 0-1. */
	rb_write_with_state(rb, "CDEF", 4, &overflow);
	CHECK(overflow == 0);
	CHECK(rb_used(rb) == 4);

	/* No room left (remain_in_seg == 0): forces growth while wrapped. */
	size_t w = rb_write_with_state(rb, "G", 1, &overflow);
	CHECK(w == 1);
	CHECK(overflow == 0);
	CHECK(rb_used(rb) == 5);

	char out[5] = { 0 };
	CHECK(rb_read(rb, out, 5) == 5);
	CHECK(memcmp(out, "CDEFG", 5) == 0);

	rb_free(rb);
}

/* -------- overflow: growth refused, ring must truncate instead of corrupt -------- */

static void test_write_overflow_evicts_oldest_data(void)
{
	/* size == size_max: no growth is ever possible. A second full write
	 * must evict all of the first write's data to make room (fill stays
	 * bounded at size_max), and flag the overflow. */
	KLRingBuffer *rb = rb_new(4, 4);
	int overflow = 0;

	rb_write_with_state(rb, "ABCD", 4, &overflow);
	CHECK(overflow == 0);

	size_t w = rb_write_with_state(rb, "EFGH", 4, &overflow);
	CHECK(w == 4);
	CHECK(overflow == 1);
	CHECK(rb_used(rb) == 4); /* capped, not 8 */

	char out[4] = { 0 };
	CHECK(rb_read(rb, out, 4) == 4);
	CHECK(memcmp(out, "EFGH", 4) == 0); /* old data was fully evicted */

	rb_free(rb);
}

/* Regression test for bug #1: a single write larger than the ring can ever
 * physically hold (bigger even than size_max) used to overflow the heap
 * allocation. It must instead truncate to the trailing portion of the
 * input, matching the header's documented "ring WILL truncate data and
 * flag an overflow condition" contract. */
static void test_write_overflow_truncates_to_trailing_bytes(void)
{
	KLRingBuffer *rb = rb_new(4, 4);
	int overflow = 0;

	size_t w = rb_write_with_state(rb, "0123456789", 10, &overflow);
	CHECK(w == 10); /* contract: always reports the requested byte count */
	CHECK(overflow == 1);
	CHECK(rb_is_full(rb));
	CHECK(rb_used(rb) == 4);

	char out[4] = { 0 };
	CHECK(rb_read(rb, out, 4) == 4);
	CHECK(memcmp(out, "6789", 4) == 0); /* only the trailing 4 bytes survive */

	rb_free(rb);
}

/* -------- rb_discard() -------- */

static void test_discard_partial(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	int overflow = 0;
	rb_write_with_state(rb, "abcdef", 6, &overflow);

	rb_discard(rb, 2); /* drop "ab" */
	CHECK(rb_used(rb) == 4);

	char out[4] = { 0 };
	CHECK(rb_read(rb, out, 4) == 4);
	CHECK(memcmp(out, "cdef", 4) == 0);

	rb_free(rb);
}

/* Regression test for bug #2: discarding more than rb_used() must clamp to
 * zero, not underflow the unsigned fill counter into a huge value. */
static void test_discard_more_than_used_clamps_to_zero(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	int overflow = 0;
	rb_write_with_state(rb, "ab", 2, &overflow);

	rb_discard(rb, 100);
	CHECK(rb_used(rb) == 0);
	CHECK(rb_is_empty(rb));

	/* Buffer must still be sanely usable afterwards. */
	size_t w = rb_write_with_state(rb, "cd", 2, &overflow);
	CHECK(w == 2);
	CHECK(rb_used(rb) == 2);

	rb_free(rb);
}

/* -------- rb_get_write_pos() / rb_get_read_pos() -------- */

static void test_write_and_read_positions(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	CHECK(rb_get_read_pos(rb) == 0);
	CHECK(rb_get_write_pos(rb) == 0);

	int overflow = 0;
	rb_write_with_state(rb, "abc", 3, &overflow);
	CHECK(rb_get_read_pos(rb) == 0);  /* head unchanged by writes */
	CHECK(rb_get_write_pos(rb) == 3); /* (head + fill) % size */

	char out[2];
	rb_read(rb, out, 2);
	CHECK(rb_get_read_pos(rb) == 2);  /* head advances on read */
	CHECK(rb_get_write_pos(rb) == 3);

	rb_free(rb);
}

/* -------- rb_fwrite() -------- */

static void test_fwrite_empty_ring_writes_nothing(void)
{
	KLRingBuffer *rb = rb_new(8, 8);
	FILE *tmp = tmpfile();
	CHECK(tmp != NULL);

	rb_fwrite(rb, tmp);
	CHECK(ftell(tmp) == 0);

	fclose(tmp);
	rb_free(rb);
}

static void test_fwrite_format_and_drains_ring(void)
{
	KLRingBuffer *rb = rb_new(16, 16);
	int overflow = 0;
	rb_write_with_state(rb, "HELLO", 5, &overflow);

	FILE *tmp = tmpfile();
	CHECK(tmp != NULL);
	if (!tmp) {
		rb_free(rb);
		return;
	}

	rb_fwrite(rb, tmp);
	CHECK(rb_is_empty(rb)); /* rb_fwrite() drains via rb_read() internally */

	long len = ftell(tmp);
	CHECK(len == (long)(4 + 4 + 5 + 4)); /* "HEAD" + u32 length + payload + "TAIL" */

	fseek(tmp, 0, SEEK_SET);
	unsigned char buf[64] = { 0 };
	size_t n = fread(buf, 1, sizeof(buf), tmp);
	CHECK(n == (size_t)len);

	CHECK(memcmp(buf, "HEAD", 4) == 0);
	uint32_t hdrlen = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
	CHECK(hdrlen == 5);
	CHECK(memcmp(buf + 8, "HELLO", 5) == 0);
	CHECK(memcmp(buf + 13, "TAIL", 4) == 0);

	fclose(tmp);
	rb_free(rb);
}

int main(void)
{
	test_new_rejects_zero_size();
	test_new_rejects_size_greater_than_max();
	test_new_accepts_size_equal_to_max();
	test_new_threadsafe_basic();
	test_new_threadsafe_rejects_zero_size();
	test_free_null_is_noop();

	test_is_empty();
	test_is_full();
	test_used_and_unused();
	test_empty_resets_without_freeing();

	test_write_read_roundtrip();
	test_deprecated_write_wrapper();
	test_peek_does_not_drain();
	test_read_more_than_available_clamps();
	test_read_from_empty_returns_zero();
	test_read_alloc();
	test_read_alloc_rejects_null_args();
	test_wraparound_write_and_read();
	test_write_exact_capacity_after_rotation();

	test_write_grows_buffer_within_max();
	test_write_grows_correctly_while_wrapped();

	test_write_overflow_evicts_oldest_data();
	test_write_overflow_truncates_to_trailing_bytes();

	test_discard_partial();
	test_discard_more_than_used_clamps_to_zero();

	test_write_and_read_positions();

	test_fwrite_empty_ring_writes_nothing();
	test_fwrite_format_and_drains_ring();

	if (g_failures == 0) {
		printf("PASS: all klringbuffer tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d klringbuffer test(s) failed\n", g_failures);
	return 1;
}
