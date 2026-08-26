/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for ltntstools_checkCRC32() / ltntstools_getCRC32().
 * Builds directly against ../src/crc32.c, no other library dependencies.
 *
 * Known-answer CRC values below were independently generated from the
 * standard CRC-32/MPEG-2 definition (poly 0x04c11db7, init 0xffffffff,
 * non-reflected, no output xor) and cross checked against the table in
 * src/crc32.c, including the well known check value 0x0376e6e7 for the
 * ASCII string "123456789".
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/crc32.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

struct kat_s {
	const char *name;
	const uint8_t *data;
	int len;
	uint32_t crc;
};

static const uint8_t v_123456789[] = "123456789";
static const uint8_t v_binary[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
static const uint8_t v_A[] = "A";
static const uint8_t v_sentence[] = "The quick brown fox jumps over the lazy dog";

static const struct kat_s g_kats[] = {
	{ "123456789",       v_123456789, sizeof(v_123456789) - 1, 0x0376e6e7 },
	{ "binary sequence", v_binary,    sizeof(v_binary),        0x4acadc12 },
	{ "single byte",     v_A,         sizeof(v_A) - 1,         0x7e4fd274 },
	{ "sentence",        v_sentence,  sizeof(v_sentence) - 1,  0xba62119e },
};

/* ltntstools_getCRC32() -- known-answer values */
static void test_getCRC32_known_answers(void)
{
	for (size_t i = 0; i < sizeof(g_kats) / sizeof(g_kats[0]); i++) {
		const struct kat_s *k = &g_kats[i];
		uint32_t crc = 0;
		int ret = ltntstools_getCRC32(k->data, k->len, &crc);
		CHECK(ret == 0);
		if (crc != k->crc) {
			fprintf(stderr, "FAIL: %s: got 0x%08x expected 0x%08x\n", k->name, crc, k->crc);
			g_failures++;
		}
	}
}

/* ltntstools_getCRC32() -- invalid argument handling */
static void test_getCRC32_invalid_args(void)
{
	uint32_t crc = 0xdeadbeef;

	/* NULL buffer */
	CHECK(ltntstools_getCRC32(NULL, 8, &crc) < 0);

	/* Zero / negative length */
	CHECK(ltntstools_getCRC32(v_binary, 0, &crc) < 0);
	CHECK(ltntstools_getCRC32(v_binary, -1, &crc) < 0);

	/* NULL output pointer */
	CHECK(ltntstools_getCRC32(v_binary, sizeof(v_binary), NULL) < 0);

	/* On error, output param must be left untouched */
	CHECK(crc == 0xdeadbeef);

	/* Minimum valid length (1 byte) succeeds */
	uint32_t out = 0;
	CHECK(ltntstools_getCRC32(v_A, 1, &out) == 0);
}

/* ltntstools_checkCRC32() -- valid buffers (payload + correct trailing CRC32) */
static void test_checkCRC32_valid(void)
{
	for (size_t i = 0; i < sizeof(g_kats) / sizeof(g_kats[0]); i++) {
		const struct kat_s *k = &g_kats[i];

		uint8_t buf[64];
		assert(k->len + 4 <= (int)sizeof(buf));
		memcpy(buf, k->data, k->len);

		/* Append the known CRC, MSB first, as ltntstools_checkCRC32() expects. */
		buf[k->len + 0] = (k->crc >> 24) & 0xff;
		buf[k->len + 1] = (k->crc >> 16) & 0xff;
		buf[k->len + 2] = (k->crc >>  8) & 0xff;
		buf[k->len + 3] = (k->crc >>  0) & 0xff;

		int ret = ltntstools_checkCRC32(buf, k->len + 4);
		if (ret != 0) {
			fprintf(stderr, "FAIL: %s: checkCRC32 rejected a valid buffer (ret=%d)\n", k->name, ret);
			g_failures++;
		}
	}
}

/* ltntstools_checkCRC32() -- corrupted buffer must be rejected */
static void test_checkCRC32_corruption_detected(void)
{
	const struct kat_s *k = &g_kats[0];

	uint8_t buf[64];
	memcpy(buf, k->data, k->len);
	buf[k->len + 0] = (k->crc >> 24) & 0xff;
	buf[k->len + 1] = (k->crc >> 16) & 0xff;
	buf[k->len + 2] = (k->crc >>  8) & 0xff;
	buf[k->len + 3] = (k->crc >>  0) & 0xff;

	/* Flip a bit in the payload -- CRC should no longer validate. */
	buf[0] ^= 0xff;
	CHECK(ltntstools_checkCRC32(buf, k->len + 4) < 0);

	/* Flip a bit in the trailing CRC itself -- should also be rejected. */
	buf[0] ^= 0xff; /* restore payload */
	buf[k->len + 3] ^= 0x01;
	CHECK(ltntstools_checkCRC32(buf, k->len + 4) < 0);
}

/* ltntstools_checkCRC32() -- invalid argument handling */
static void test_checkCRC32_invalid_args(void)
{
	uint8_t buf[8] = { 0 };

	/* NULL buffer */
	CHECK(ltntstools_checkCRC32(NULL, 8) < 0);

	/* Too short to even hold a CRC32 trailer */
	CHECK(ltntstools_checkCRC32(buf, 0) < 0);
	CHECK(ltntstools_checkCRC32(buf, 3) < 0);
	CHECK(ltntstools_checkCRC32(buf, -1) < 0);
}

int main(void)
{
	test_getCRC32_known_answers();
	test_getCRC32_invalid_args();
	test_checkCRC32_valid();
	test_checkCRC32_corruption_detected();
	test_checkCRC32_invalid_args();

	if (g_failures == 0) {
		printf("PASS: all crc32 tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d crc32 test(s) failed\n", g_failures);
	return 1;
}
