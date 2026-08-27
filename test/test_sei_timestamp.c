/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/sei-timestamp.c / src/sei-timestamp.h. This is a
 * library-private header (no libltntstools/sei-timestamp.h), included
 * directly here exactly as src/sei-timestamp.c itself does.
 *
 * Builds against ../src/sei-timestamp.c only -- ltn_memmem() (memmem.h) and
 * ltn_timeval_subtract_ms() (libltntstools/timeval.h) are both header-only
 * inline/static functions, no separate .o needed.
 *
 * A REAL out-of-bounds-read bug was found while writing these tests and has
 * been FIXED in src/sei-timestamp.c: sei_timestamp_field_get() took a
 * `lengthBytes` parameter but never used it anywhere in its body, unlike
 * its write-side counterpart sei_timestamp_field_set(), which validates
 * `lengthBytes - (p - buffer) < 6` before touching memory. Any caller
 * asking for a field number whose 6-byte slot falls beyond the buffer they
 * actually provided (e.g. a truncated/corrupted SEI payload parsed from a
 * live, untrusted transport stream -- exactly this function's real usage)
 * got a silent heap over-read instead of an error. Confirmed via repro: a
 * 22-byte buffer (UUID + field 1 only) with sei_timestamp_field_get(buf,
 * 22, 9, &v) returned 0 (success) and read 48 bytes past the buffer before
 * the fix; now correctly returns -1. Fixed by adding the same bounds check
 * sei_timestamp_field_set() already had.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "sei-timestamp.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- sei_timestamp_alloc / sei_timestamp_init -------- */

static void test_alloc_sets_uuid_prefix_and_zeroes_rest(void)
{
	unsigned char *buf = sei_timestamp_alloc();
	CHECK(buf != NULL);
	if (!buf)
		return;

	CHECK(memcmp(buf, ltn_uuid_sei_timestamp, sizeof(ltn_uuid_sei_timestamp)) == 0);

	for (unsigned int i = sizeof(ltn_uuid_sei_timestamp); i < SEI_TIMESTAMP_PAYLOAD_LENGTH; i++) {
		CHECK(buf[i] == 0);
	}

	free(buf);
}

static void test_init_too_small_buffer_fails(void)
{
	unsigned char buf[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	memset(buf, 0xAA, sizeof(buf));

	CHECK(sei_timestamp_init(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH - 1) == -1);
	/* Rejected before touching the buffer. */
	CHECK(buf[0] == 0xAA);
}

static void test_init_success_sets_uuid_and_zeros_rest(void)
{
	unsigned char buf[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	memset(buf, 0xFF, sizeof(buf));

	CHECK(sei_timestamp_init(buf, (int)sizeof(buf)) == 0);
	CHECK(memcmp(buf, ltn_uuid_sei_timestamp, sizeof(ltn_uuid_sei_timestamp)) == 0);
	for (unsigned int i = sizeof(ltn_uuid_sei_timestamp); i < sizeof(buf); i++) {
		CHECK(buf[i] == 0);
	}
}

/* -------- sei_timestamp_field_set -------- */

static void test_field_set_invalid_nr_rejected(void)
{
	unsigned char buf[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	sei_timestamp_init(buf, sizeof(buf));

	CHECK(sei_timestamp_field_set(buf, sizeof(buf), 0, 0x11223344) == -1);
	CHECK(sei_timestamp_field_set(buf, sizeof(buf), SEI_TIMESTAMP_FIELD_COUNT + 1, 0x11223344) == -1);
}

static void test_field_set_rejects_when_buffer_too_small_for_field(void)
{
	/* Only room for the UUID plus field 1 (22 bytes); field 2 needs 6 more. */
	unsigned char buf[16 + 6];
	memset(buf, 0, sizeof(buf));
	memcpy(buf, ltn_uuid_sei_timestamp, sizeof(ltn_uuid_sei_timestamp));

	CHECK(sei_timestamp_field_set(buf, sizeof(buf), 1, 0x11223344) == 0);
	CHECK(sei_timestamp_field_set(buf, sizeof(buf), 2, 0x11223344) == -1);
}

static void test_field_set_writes_expected_byte_layout_with_delimiters(void)
{
	unsigned char buf[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	sei_timestamp_init(buf, sizeof(buf));

	CHECK(sei_timestamp_field_set(buf, sizeof(buf), 1, 0x11223344) == 0);

	unsigned char *p = buf + sizeof(ltn_uuid_sei_timestamp);
	CHECK(p[0] == 0x11);
	CHECK(p[1] == 0x22);
	CHECK(p[2] == SEI_BIT_DELIMITER);
	CHECK(p[3] == 0x33);
	CHECK(p[4] == 0x44);
	CHECK(p[5] == SEI_BIT_DELIMITER);
}

/* -------- sei_timestamp_field_get -------- */

static void test_field_get_invalid_nr_rejected(void)
{
	unsigned char buf[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	sei_timestamp_init(buf, sizeof(buf));

	uint32_t value = 0xdeadbeef;
	CHECK(sei_timestamp_field_get(buf, sizeof(buf), 0, &value) == -1);
	CHECK(value == 0xdeadbeef); /* untouched */
	CHECK(sei_timestamp_field_get(buf, sizeof(buf), SEI_TIMESTAMP_FIELD_COUNT + 1, &value) == -1);
	CHECK(value == 0xdeadbeef);
}

/* Regression test for the fixed OOB-read bug: see file header. */
static void test_field_get_rejects_when_buffer_too_small_for_field(void)
{
	unsigned char buf[16 + 6]; /* UUID + field 1 only */
	memset(buf, 0, sizeof(buf));
	memcpy(buf, ltn_uuid_sei_timestamp, sizeof(ltn_uuid_sei_timestamp));
	sei_timestamp_field_set(buf, sizeof(buf), 1, 0x01020304);

	uint32_t value = 0;
	CHECK(sei_timestamp_field_get(buf, sizeof(buf), 1, &value) == 0);
	CHECK(value == 0x01020304);

	/* Field 9's 6-byte slot starts at offset 16+8*6=64, well past this
	 * 22-byte buffer -- must be rejected, not read out of bounds. */
	value = 0xdeadbeef;
	CHECK(sei_timestamp_field_get(buf, sizeof(buf), SEI_TIMESTAMP_FIELD_COUNT, &value) == -1);
	CHECK(value == 0xdeadbeef);
}

static void test_field_set_get_roundtrip_all_fields(void)
{
	unsigned char *buf = sei_timestamp_alloc();
	CHECK(buf != NULL);
	if (!buf)
		return;

	for (uint32_t nr = 1; nr <= SEI_TIMESTAMP_FIELD_COUNT; nr++) {
		uint32_t written = 0x10000000 * nr + nr;
		CHECK(sei_timestamp_field_set(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, nr, written) == 0);

		uint32_t readback = 0;
		CHECK(sei_timestamp_field_get(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, nr, &readback) == 0);
		CHECK(readback == written);
	}

	free(buf);
}

/* -------- ltn_uuid_find -------- */

static void test_uuid_find_locates_uuid_at_nonzero_offset(void)
{
	unsigned char buf[8 + SEI_TIMESTAMP_PAYLOAD_LENGTH];
	memset(buf, 0x00, 8);
	memcpy(buf + 8, ltn_uuid_sei_timestamp, sizeof(ltn_uuid_sei_timestamp));

	int idx = ltn_uuid_find(buf, sizeof(buf));
	CHECK(idx == 8);
}

static void test_uuid_find_not_present_returns_negative(void)
{
	unsigned char buf[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	memset(buf, 0x00, sizeof(buf));

	CHECK(ltn_uuid_find(buf, sizeof(buf)) == -1);
}

static void test_uuid_find_buffer_shorter_than_payload_returns_negative(void)
{
	/* Actual code requires the WHOLE payload length to even attempt a
	 * search, even if the UUID itself would technically fit. */
	unsigned char buf[SEI_TIMESTAMP_PAYLOAD_LENGTH];
	memcpy(buf, ltn_uuid_sei_timestamp, sizeof(ltn_uuid_sei_timestamp));

	CHECK(ltn_uuid_find(buf, SEI_TIMESTAMP_PAYLOAD_LENGTH - 1) == -1);
}

/* -------- sei_timestamp_query_codec_latency_ms -------- */

static void test_query_codec_latency_ms_computes_difference(void)
{
	unsigned char *buf = sei_timestamp_alloc();
	CHECK(buf != NULL);
	if (!buf)
		return;

	/* Field 4/5 = compressor entry (sec/usec), field 6/7 = compressor exit. */
	sei_timestamp_field_set(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 4, 1000);
	sei_timestamp_field_set(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 5, 0);
	sei_timestamp_field_set(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, 1000);
	sei_timestamp_field_set(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 7, 250000);

	int64_t ms = sei_timestamp_query_codec_latency_ms(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH);
	CHECK(ms == 250);

	free(buf);
}

/* -------- sei_timestamp_value_timeval_set / _query -------- */

static void test_value_timeval_set_and_query_roundtrip(void)
{
	unsigned char *buf = sei_timestamp_alloc();
	CHECK(buf != NULL);
	if (!buf)
		return;

	struct timeval in = { .tv_sec = 123456, .tv_usec = 789 };
	CHECK(sei_timestamp_value_timeval_set(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 2, &in) == 0);

	struct timeval out = { 0, 0 };
	CHECK(sei_timestamp_value_timeval_query(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 2, &out) == 0);

	CHECK(out.tv_sec == in.tv_sec);
	CHECK(out.tv_usec == in.tv_usec);

	free(buf);
}

static void test_value_timeval_set_null_uses_now(void)
{
	unsigned char *buf = sei_timestamp_alloc();
	CHECK(buf != NULL);
	if (!buf)
		return;

	struct timeval before, after;
	gettimeofday(&before, NULL);
	CHECK(sei_timestamp_value_timeval_set(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, NULL) == 0);
	gettimeofday(&after, NULL);

	struct timeval out = { 0, 0 };
	sei_timestamp_value_timeval_query(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, &out);

	CHECK(out.tv_sec >= before.tv_sec && out.tv_sec <= after.tv_sec);

	free(buf);
}

/* -------- sei_timestamp_hexdump -------- */

static void test_hexdump_does_not_crash(void)
{
	unsigned char *buf = sei_timestamp_alloc();
	CHECK(buf != NULL);
	if (buf) {
		sei_timestamp_hexdump(buf, (int)SEI_TIMESTAMP_PAYLOAD_LENGTH);
		free(buf);
	}
	CHECK(1);
}

int main(void)
{
	test_alloc_sets_uuid_prefix_and_zeroes_rest();
	test_init_too_small_buffer_fails();
	test_init_success_sets_uuid_and_zeros_rest();

	test_field_set_invalid_nr_rejected();
	test_field_set_rejects_when_buffer_too_small_for_field();
	test_field_set_writes_expected_byte_layout_with_delimiters();

	test_field_get_invalid_nr_rejected();
	test_field_get_rejects_when_buffer_too_small_for_field();
	test_field_set_get_roundtrip_all_fields();

	test_uuid_find_locates_uuid_at_nonzero_offset();
	test_uuid_find_not_present_returns_negative();
	test_uuid_find_buffer_shorter_than_payload_returns_negative();

	test_query_codec_latency_ms_computes_difference();

	test_value_timeval_set_and_query_roundtrip();
	test_value_timeval_set_null_uses_now();

	test_hexdump_does_not_crash();

	if (g_failures == 0) {
		printf("PASS: all sei-timestamp tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d sei-timestamp test(s) failed\n", g_failures);
	return 1;
}
