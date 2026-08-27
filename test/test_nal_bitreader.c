/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/nal_bitreader.c / src/libltntstools/nal_bitreader.h.
 * Header-only declared, single .c implementation, no other dependencies --
 * this test file needs nothing beyond nal_bitreader.c itself.
 *
 * TWO REAL bugs were found while writing these tests and have been FIXED
 * in src/nal_bitreader.c:
 *
 * 1. (Confirmed via UBSan) NALBitReader_read_ue()'s guard against an
 *    over-long Exp-Golomb prefix was `if (leadingZeroBits > 31) return -1;`
 *    -- but leadingZeroBits == 31 was still allowed through to
 *    `(1 << leadingZeroBits) - 1 + infoBits`. `1 << 31` shifts a 1 into a
 *    signed int's sign bit (undefined behavior), and the following `- 1`
 *    is then also a signed integer overflow (UBSan: "left shift of 1 by
 *    31 places cannot be represented in type 'int'", then "signed integer
 *    overflow: -2147483648 - 1 cannot be represented in type 'int'").
 *    Confirmed via repro: a stream with exactly 31 leading zero bits
 *    before the stop bit hit both UB reports and returned 2147483647.
 *    Fixed by rejecting leadingZeroBits == 31 too (`>= 31`); no
 *    legitimate H.264/H.265 Exp-Golomb code needs that many leading
 *    zeros. test_read_ue_31_leading_zeros_is_rejected_not_ub() below is
 *    the regression test.
 *
 * 2. NALBitReader_read_se() didn't check whether the underlying
 *    NALBitReader_read_ue() call it wraps had failed (returned -1, its
 *    documented error sentinel) before applying the unsigned-to-signed
 *    Exp-Golomb mapping. Since -1 (all bits set) is odd, the mapping
 *    silently computed `((-1) + 1) >> 1 == 0` -- a failed/truncated read
 *    was indistinguishable from the genuinely valid signed value 0, with
 *    no way for a caller to detect the underlying corruption. Confirmed
 *    via repro: read_se() on an already-exhausted reader returned 0
 *    instead of an error. Fixed by propagating read_ue()'s negative
 *    return value directly. test_read_se_propagates_read_ue_failure()
 *    below is the regression test.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/nal_bitreader.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- init / skip_bits / skip_to_byte_aligned -------- */

static void test_init_sets_fields(void)
{
	unsigned char buf[4] = { 0xAB, 0xCD, 0xEF, 0x01 };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(br.data == buf);
	CHECK(br.size == 4);
	CHECK(br.bit_pos == 0);
}

static void test_skip_bits_advances_position(void)
{
	unsigned char buf[2] = { 0xF0, 0x0F }; /* 11110000 00001111 */
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	NALBitReader_skip_bits(&br, 4);
	CHECK(br.bit_pos == 4);
	CHECK(NALBitReader_read_bit(&br) == 0); /* 5th bit of 0xF0 is 0 */
}

static void test_skip_bits_past_end_is_safe(void)
{
	unsigned char buf[1] = { 0xFF };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	NALBitReader_skip_bits(&br, 100); /* far past the 8 available bits */
	CHECK(NALBitReader_read_bit(&br) == -1); /* still safely exhausted */
}

static void test_skip_to_byte_aligned_from_various_offsets(void)
{
	unsigned char buf[4] = { 0, 0, 0, 0 };
	NALBitReader br;

	NALBitReader_init(&br, buf, sizeof(buf));
	NALBitReader_skip_to_byte_aligned(&br); /* already aligned -> no-op */
	CHECK(br.bit_pos == 0);

	NALBitReader_init(&br, buf, sizeof(buf));
	NALBitReader_skip_bits(&br, 3);
	NALBitReader_skip_to_byte_aligned(&br);
	CHECK(br.bit_pos == 8);

	NALBitReader_init(&br, buf, sizeof(buf));
	NALBitReader_skip_bits(&br, 8);
	NALBitReader_skip_to_byte_aligned(&br); /* exactly aligned -> no-op */
	CHECK(br.bit_pos == 8);

	NALBitReader_init(&br, buf, sizeof(buf));
	NALBitReader_skip_bits(&br, 15);
	NALBitReader_skip_to_byte_aligned(&br);
	CHECK(br.bit_pos == 16);
}

/* -------- read_bit -------- */

static void test_read_bit_msb_first_order(void)
{
	unsigned char buf[1] = { 0b10110010 };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	int expected[8] = { 1, 0, 1, 1, 0, 0, 1, 0 };
	for (int i = 0; i < 8; i++) {
		CHECK(NALBitReader_read_bit(&br) == expected[i]);
	}
	CHECK(br.bit_pos == 8);
}

static void test_read_bit_across_byte_boundary(void)
{
	unsigned char buf[2] = { 0x01, 0x80 }; /* 00000001 10000000 */
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	NALBitReader_skip_bits(&br, 7);
	CHECK(NALBitReader_read_bit(&br) == 1); /* last bit of byte 0 */
	CHECK(NALBitReader_read_bit(&br) == 1); /* first bit of byte 1 */
	CHECK(NALBitReader_read_bit(&br) == 0);
}

static void test_read_bit_returns_negative_one_past_end(void)
{
	unsigned char buf[1] = { 0xFF };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	for (int i = 0; i < 8; i++)
		CHECK(NALBitReader_read_bit(&br) == 1);

	CHECK(NALBitReader_read_bit(&br) == -1);
	CHECK(NALBitReader_read_bit(&br) == -1); /* repeated calls stay safe */
	CHECK(br.bit_pos == 8); /* position doesn't advance past the end */
}

static void test_read_bit_empty_buffer(void)
{
	NALBitReader br;
	NALBitReader_init(&br, NULL, 0);
	CHECK(NALBitReader_read_bit(&br) == -1);
}

/* -------- read_bits -------- */

static void test_read_bits_various_widths(void)
{
	unsigned char buf[4] = { 0xAB, 0xCD, 0xEF, 0x12 };
	NALBitReader br;

	NALBitReader_init(&br, buf, sizeof(buf));
	CHECK(NALBitReader_read_bits(&br, 8) == 0xAB);
	CHECK(NALBitReader_read_bits(&br, 8) == 0xCD);

	NALBitReader_init(&br, buf, sizeof(buf));
	CHECK(NALBitReader_read_bits(&br, 16) == 0xABCD);

	NALBitReader_init(&br, buf, sizeof(buf));
	CHECK(NALBitReader_read_bits(&br, 32) == 0xABCDEF12);

	NALBitReader_init(&br, buf, sizeof(buf));
	CHECK(NALBitReader_read_bits(&br, 4) == 0xA);
	CHECK(NALBitReader_read_bits(&br, 4) == 0xB);
}

static void test_read_bits_zero_width_returns_zero(void)
{
	unsigned char buf[1] = { 0xFF };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_bits(&br, 0) == 0);
	CHECK(br.bit_pos == 0); /* untouched */
}

static void test_read_bits_past_end_returns_sentinel(void)
{
	unsigned char buf[1] = { 0xFF };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_bits(&br, 16) == 0xFFFFFFFF); /* only 8 bits available */
}

/* -------- read_ue (unsigned Exp-Golomb) -------- */

static void test_read_ue_known_codes(void)
{
	/* Standard Exp-Golomb prefix table: codeNum 0->"1", 1->"010",
	 * 2->"011", 3->"00100", 4->"00101", 5->"00110", 6->"00111". */
	struct { const char *bits; int expected; } cases[] = {
		{ "1", 0 },
		{ "010", 1 },
		{ "011", 2 },
		{ "00100", 3 },
		{ "00101", 4 },
		{ "00110", 5 },
		{ "00111", 6 },
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		const char *bits = cases[c].bits;
		int nbits = (int)strlen(bits);
		unsigned char buf[4] = { 0 };
		for (int i = 0; i < nbits; i++) {
			if (bits[i] == '1')
				buf[i / 8] |= (0x80 >> (i % 8));
		}

		NALBitReader br;
		NALBitReader_init(&br, buf, sizeof(buf));
		int v = NALBitReader_read_ue(&br);
		CHECK(v == cases[c].expected);
		CHECK(br.bit_pos == nbits); /* consumed exactly the prefix+info bits */
	}
}

static void test_read_ue_fails_on_truncated_stream(void)
{
	/* All zero bits, no terminating '1' anywhere before the buffer ends. */
	unsigned char buf[2] = { 0x00, 0x00 };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_ue(&br) == -1);
}

/* Regression test for the fixed UB bug: see file header. */
static void test_read_ue_31_leading_zeros_is_rejected_not_ub(void)
{
	unsigned char buf[16] = { 0 };
	buf[3] = 0x01; /* the 32nd bit (index 31) is the '1' stop bit */

	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_ue(&br) == -1);
}

static void test_read_ue_30_leading_zeros_still_works(void)
{
	/* One fewer leading zero than the rejected case: must succeed. */
	unsigned char buf[16] = { 0 };
	buf[3] = 0x02; /* bit index 30 is the '1' stop bit */

	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	int v = NALBitReader_read_ue(&br);
	CHECK(v == (1 << 30) - 1); /* 30 leading zeros, 30 zero info bits */
}

/* -------- read_se (signed Exp-Golomb) -------- */

static void test_read_se_known_mapping(void)
{
	/* codeNum -> se(v): 0->0, 1->1, 2->-1, 3->2, 4->-2, 5->3, 6->-3 */
	struct { const char *bits; int expected; } cases[] = {
		{ "1", 0 },
		{ "010", 1 },
		{ "011", -1 },
		{ "00100", 2 },
		{ "00101", -2 },
		{ "00110", 3 },
		{ "00111", -3 },
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		const char *bits = cases[c].bits;
		int nbits = (int)strlen(bits);
		unsigned char buf[4] = { 0 };
		for (int i = 0; i < nbits; i++) {
			if (bits[i] == '1')
				buf[i / 8] |= (0x80 >> (i % 8));
		}

		NALBitReader br;
		NALBitReader_init(&br, buf, sizeof(buf));
		CHECK(NALBitReader_read_se(&br) == cases[c].expected);
	}
}

/* Regression test for the fixed failure-masking bug: see file header. */
static void test_read_se_propagates_read_ue_failure(void)
{
	unsigned char buf[2] = { 0x00, 0x00 }; /* exhausts with no stop bit */
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_se(&br) == -1);
}

/* -------- read_me (mapped Exp-Golomb, coded_block_pattern) -------- */

static void test_read_me_inter_and_intra_tables(void)
{
	/* codeNum 0 -> "1" prefix. cbp_inter[0]=0, cbp_intra[0]=47. */
	unsigned char buf[1] = { 0x80 }; /* "1" then padding */

	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));
	CHECK(NALBitReader_read_me(&br, 0 /* inter */) == 0);

	NALBitReader_init(&br, buf, sizeof(buf));
	CHECK(NALBitReader_read_me(&br, 1 /* intra */) == 47);
}

static void test_read_me_out_of_range_codenum_fails(void)
{
	/* codeNum 3 -> "00100" prefix, corresponds to leadingZeroBits=2,
	 * infoBits=00 -> codeNum=3, well within [0,47], sanity-check this
	 * first, then confirm a codeNum >= 48 (out of table range) fails. */
	unsigned char inRange[1] = { 0b00100000 };
	NALBitReader br;
	NALBitReader_init(&br, inRange, sizeof(inRange));
	CHECK(NALBitReader_read_me(&br, 0) == 2); /* cbp_inter[3] == 2 */

	/* codeNum 48: leadingZeroBits=5, "000000" + 5 info bits all 1
	 * (11111) -> codeNum = (1<<5)-1+31 = 31+31 = 62... build precisely
	 * via read_ue known-good encoding instead: 6 leading zeros then a
	 * '1' then 6 info bits value 17 -> codeNum = (1<<6)-1+17 = 80 >= 48. */
	unsigned char outOfRange[2] = { 0b00000001, 0b00010001 };
	NALBitReader_init(&br, outOfRange, sizeof(outOfRange));
	int codeNum = NALBitReader_read_ue(&br);
	CHECK(codeNum >= 48); /* sanity: this construction is actually out of range */

	NALBitReader_init(&br, outOfRange, sizeof(outOfRange));
	CHECK(NALBitReader_read_me(&br, 0) == -1);
}

static void test_read_me_failed_ue_propagates(void)
{
	unsigned char buf[2] = { 0x00, 0x00 };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_me(&br, 0) == -1);
}

/* -------- read_te (truncated Exp-Golomb) -------- */

static void test_read_te_maxval_zero_returns_zero_untouched(void)
{
	unsigned char buf[1] = { 0xFF };
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_te(&br, 0) == 0);
	CHECK(br.bit_pos == 0); /* doesn't touch the bitstream */
}

static void test_read_te_maxval_one_inverts_single_bit(void)
{
	unsigned char zero[1] = { 0x00 };
	NALBitReader br;
	NALBitReader_init(&br, zero, sizeof(zero));
	CHECK(NALBitReader_read_te(&br, 1) == 1); /* bit=0 -> value 1 */

	unsigned char one[1] = { 0x80 };
	NALBitReader_init(&br, one, sizeof(one));
	CHECK(NALBitReader_read_te(&br, 1) == 0); /* bit=1 -> value 0 */
}

static void test_read_te_maxval_one_fails_on_exhausted_stream(void)
{
	NALBitReader br;
	NALBitReader_init(&br, NULL, 0);
	CHECK(NALBitReader_read_te(&br, 1) == -1);
}

static void test_read_te_maxval_above_one_uses_ue(void)
{
	unsigned char buf[1] = { 0b01000000 }; /* "010" -> ue=1 */
	NALBitReader br;
	NALBitReader_init(&br, buf, sizeof(buf));

	CHECK(NALBitReader_read_te(&br, 5) == 1);
}

int main(void)
{
	test_init_sets_fields();
	test_skip_bits_advances_position();
	test_skip_bits_past_end_is_safe();
	test_skip_to_byte_aligned_from_various_offsets();

	test_read_bit_msb_first_order();
	test_read_bit_across_byte_boundary();
	test_read_bit_returns_negative_one_past_end();
	test_read_bit_empty_buffer();

	test_read_bits_various_widths();
	test_read_bits_zero_width_returns_zero();
	test_read_bits_past_end_returns_sentinel();

	test_read_ue_known_codes();
	test_read_ue_fails_on_truncated_stream();
	test_read_ue_31_leading_zeros_is_rejected_not_ub();
	test_read_ue_30_leading_zeros_still_works();

	test_read_se_known_mapping();
	test_read_se_propagates_read_ue_failure();

	test_read_me_inter_and_intra_tables();
	test_read_me_out_of_range_codenum_fails();
	test_read_me_failed_ue_propagates();

	test_read_te_maxval_zero_returns_zero_untouched();
	test_read_te_maxval_one_inverts_single_bit();
	test_read_te_maxval_one_fails_on_exhausted_stream();
	test_read_te_maxval_above_one_uses_ue();

	if (g_failures == 0) {
		printf("PASS: all nal_bitreader tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d nal_bitreader test(s) failed\n", g_failures);
	return 1;
}
