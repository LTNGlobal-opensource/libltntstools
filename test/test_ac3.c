/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/ac3.c / src/libltntstools/ac3.h.
 * Builds against ../src/ac3.c directly (self-contained: it only needs the
 * header-only ../src/libltntstools/klbitstream_readwriter.h).
 *
 * Two real bugs were found and fixed in ltntstools_ac3_header_parse() while
 * writing this file (confirmed with AddressSanitizer before the fix):
 *  1. No NULL check on `sf`/`buf` -- calling it with buf == NULL segfaulted
 *     immediately inside the bitstream reader.
 *  2. The function only validated the buffer length once, up front
 *     (>= 22 bytes), but AC3 headers carry a self-described extra-length
 *     field (addbsil, up to 64 more bytes) and several optional sections,
 *     so the real amount of data needed can exceed that floor. The
 *     underlying bitstream reader safely stops and sets an internal
 *     `overrun` flag when that happens, but the parser never checked it,
 *     so it reported success (0) with the tail of the struct silently
 *     zero-filled. test_parse_null_args() and
 *     test_parse_reports_failure_on_truncated_extended_bsi() are direct
 *     regression coverage for bugs 1 and 2 respectively.
 *
 * TEST STRATEGY: rather than hand-crafting raw byte arrays (fragile and
 * opaque), build_ac3_header() below serializes a fully-populated
 * struct ltn_ac3_header_syncframe_s into a bitstream using the exact same
 * field order/conditions as ltntstools_ac3_header_parse() (verified against
 * ac3.c), via the same klbitstream_readwriter.h this project already has
 * its own dedicated, separate unit tests for (test_klbitstream_readwriter.c).
 * Each test round-trips a "wanted" struct through build -> parse and
 * compares the parsed result field-by-field against what was asked for.
 * Note dialnorm/dialnorm2 are stored post-negation by the parser (see
 * ac3.c), so the builder negates them back to the raw on-the-wire code.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/ac3.h"
#include "libltntstools/klbitstream_readwriter.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

#define AC3_MIN_BYTES (8 + 14)

/* See Table 5.8 in ac3.c -- duplicated here since it's private to that file. */
static const uint32_t nfchans_table[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };

/* Serializes `sf` into `out` (capacity `cap`) using the identical field
 * order/conditions as ltntstools_ac3_header_parse(). Pads with zero bytes
 * up to at least AC3_MIN_BYTES. Returns the number of bytes written, or 0
 * if `cap` was too small for the requested fields.
 */
static size_t build_ac3_header(uint8_t *out, size_t cap, const struct ltn_ac3_header_syncframe_s *sf)
{
	memset(out, 0, cap);

	struct klbs_context_s pbs, *bs = &pbs;
	klbs_init(bs);
	klbs_write_set_buffer(bs, out, cap);

	klbs_write_bits(bs, sf->syncinfo.syncword, 16);
	klbs_write_bits(bs, sf->syncinfo.crc1, 16);
	klbs_write_bits(bs, sf->syncinfo.fscod, 2);
	klbs_write_bits(bs, sf->syncinfo.frmsizecod, 6);

	klbs_write_bits(bs, sf->bsi.bsid, 5);
	klbs_write_bits(bs, sf->bsi.bsmod, 3);
	klbs_write_bits(bs, sf->bsi.acmod, 3);

	if ((sf->bsi.acmod & 0x1) && (sf->bsi.acmod != 0x1)) {
		klbs_write_bits(bs, sf->bsi.cmixlev, 2);
	}
	if (sf->bsi.acmod & 0x4) {
		klbs_write_bits(bs, sf->bsi.surmixlev, 2);
	}
	if (sf->bsi.acmod == 0x2) {
		klbs_write_bits(bs, sf->bsi.dsurmod, 2);
	}

	klbs_write_bits(bs, sf->bsi.lfeon, 1);
	klbs_write_bits(bs, (uint32_t)(-sf->bsi.dialnorm), 5);
	klbs_write_bits(bs, sf->bsi.compre, 1);
	if (sf->bsi.compre) {
		klbs_write_bits(bs, sf->bsi.compr, 8);
	}
	klbs_write_bits(bs, sf->bsi.langcode, 1);
	if (sf->bsi.langcode) {
		klbs_write_bits(bs, sf->bsi.langcod, 8);
	}
	klbs_write_bits(bs, sf->bsi.audprodie, 1);
	if (sf->bsi.audprodie) {
		klbs_write_bits(bs, sf->bsi.mixlevel, 5);
		klbs_write_bits(bs, sf->bsi.roomtyp, 2);
	}

	if (sf->bsi.acmod == 0) {
		klbs_write_bits(bs, (uint32_t)(-sf->bsi.dialnorm2), 5);
		klbs_write_bits(bs, sf->bsi.compr2e, 1);
		if (sf->bsi.compr2e) {
			klbs_write_bits(bs, sf->bsi.compr2, 8);
		}
		klbs_write_bits(bs, sf->bsi.langcod2e, 1);
		if (sf->bsi.langcod2e) {
			klbs_write_bits(bs, sf->bsi.langcod2, 8);
		}
		klbs_write_bits(bs, sf->bsi.audprodi2e, 1);
		if (sf->bsi.audprodi2e) {
			klbs_write_bits(bs, sf->bsi.mixlevel2, 5);
			klbs_write_bits(bs, sf->bsi.roomtyp2, 2);
		}
	}

	klbs_write_bits(bs, sf->bsi.copyrightb, 1);
	klbs_write_bits(bs, sf->bsi.origbs, 1);

	if (sf->bsi.bsid == 8) {
		klbs_write_bits(bs, sf->bsi.timecod1e, 1);
		if (sf->bsi.timecod1e) {
			klbs_write_bits(bs, sf->bsi.timecod1, 14);
		}
		klbs_write_bits(bs, sf->bsi.timecod2e, 1);
		if (sf->bsi.timecod2e) {
			klbs_write_bits(bs, sf->bsi.timecod2, 14);
		}
	} else if (sf->bsi.bsid == 6) {
		klbs_write_bits(bs, sf->bsi.xbsi1e, 1);
		if (sf->bsi.xbsi1e) {
			klbs_write_bits(bs, sf->bsi.dmixmod, 2);
			klbs_write_bits(bs, sf->bsi.ltrtcmixlev, 3);
			klbs_write_bits(bs, sf->bsi.ltrtsurmixlev, 3);
			klbs_write_bits(bs, sf->bsi.lorocmixlev, 3);
			klbs_write_bits(bs, sf->bsi.lorosurmixlev, 3);
		}
		klbs_write_bits(bs, sf->bsi.xbsi2e, 1);
		if (sf->bsi.xbsi2e) {
			klbs_write_bits(bs, sf->bsi.dsurexmod, 2);
			klbs_write_bits(bs, sf->bsi.dheadphonmod, 2);
			klbs_write_bits(bs, sf->bsi.adconvtyp, 1);
			klbs_write_bits(bs, sf->bsi.xbsi2, 8);
			klbs_write_bits(bs, sf->bsi.encinfo, 1);
		}
	}

	klbs_write_bits(bs, sf->bsi.addbsie, 1);
	if (sf->bsi.addbsie) {
		klbs_write_bits(bs, sf->bsi.addbsil, 6);
		for (int i = 0; i < (int)(sf->bsi.addbsil + 1); i++) {
			klbs_write_bits(bs, sf->bsi.addbsi[i], 8);
		}
	}

	uint32_t nf = nfchans_table[sf->bsi.acmod & 0x7];
	for (int i = 0; i < 6; i++) {
		const struct ltn_ac3_header_audioblk_s *b = &sf->audioblk[i];
		for (int ch = 0; ch < (int)nf; ch++) {
			klbs_write_bits(bs, b->blksw[ch], 1);
		}
		for (int ch = 0; ch < (int)nf; ch++) {
			klbs_write_bits(bs, b->dithflag[ch], 1);
		}
		klbs_write_bits(bs, b->dynrnge, 1);
		if (b->dynrnge) {
			klbs_write_bits(bs, b->dynrng, 8);
		}
	}

	klbs_write_byte_stuff(bs, 0);

	size_t used = klbs_get_byte_count(bs);
	while (used < AC3_MIN_BYTES && used < cap) {
		out[used++] = 0;
	}
	return used;
}

/* -------- parse(): argument validation -------- */

static void test_parse_null_args(void)
{
	struct ltn_ac3_header_syncframe_s sf;
	uint8_t buf[AC3_MIN_BYTES] = { 0 };

	CHECK(ltntstools_ac3_header_parse(NULL, buf, sizeof(buf)) < 0);
	CHECK(ltntstools_ac3_header_parse(&sf, NULL, sizeof(buf)) < 0);
}

static void test_parse_rejects_short_buffer(void)
{
	struct ltn_ac3_header_syncframe_s sf;
	uint8_t buf[AC3_MIN_BYTES - 1] = { 0 };

	CHECK(ltntstools_ac3_header_parse(&sf, buf, sizeof(buf)) < 0);
	CHECK(ltntstools_ac3_header_parse(&sf, buf, -1) < 0);
	CHECK(ltntstools_ac3_header_parse(&sf, buf, 0) < 0);
}

/* Regression test for bug #2: addbsil claims far more trailing data than
 * the buffer actually contains. The bitstream reader safely detects this
 * (no crash), but the parser must now report failure instead of a
 * misleadingly "successful" partially-populated struct. */
static void test_parse_reports_failure_on_truncated_extended_bsi(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 1; /* 1/0, no extra mix-level bits, keeps the header short */
	want.bsi.addbsie = 1;
	want.bsi.addbsil = 63; /* claims 64 trailing bytes we won't provide */

	uint8_t buf[AC3_MIN_BYTES]; /* deliberately too small for addbsil's claim */
	size_t used = build_ac3_header(buf, sizeof(buf), &want);
	CHECK(used == AC3_MIN_BYTES); /* builder truncated to the buffer we gave it too */

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) < 0);
}

/* -------- parse(): well-formed round-trips -------- */

static void test_parse_basic_syncinfo(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.syncinfo.crc1 = 0xbeef;
	want.syncinfo.fscod = 1; /* 44.1 KHz */
	want.syncinfo.frmsizecod = 42 & 0x3f;
	want.bsi.acmod = 2; /* 2/0, L R */

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);
	CHECK(used >= AC3_MIN_BYTES);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);

	CHECK(got.syncinfo.syncword == want.syncinfo.syncword);
	CHECK(got.syncinfo.crc1 == want.syncinfo.crc1);
	CHECK(got.syncinfo.fscod == want.syncinfo.fscod);
	CHECK(got.syncinfo.frmsizecod == want.syncinfo.frmsizecod);
	CHECK(got.bsi.acmod_nfchans == 2);
}

static void test_parse_acmod_channel_count_table(void)
{
	/* acmod -> nfchans per Table 5.8, exercised for every legal acmod
	 * value (also exercises the cmixlev/surmixlev/dsurmod gating). */
	uint32_t expected_nfchans[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };

	for (uint32_t acmod = 0; acmod <= 7; acmod++) {
		struct ltn_ac3_header_syncframe_s want;
		memset(&want, 0, sizeof(want));
		want.syncinfo.syncword = 0x0b77;
		want.bsi.acmod = acmod;
		want.bsi.cmixlev = 1;
		want.bsi.surmixlev = 1;
		want.bsi.dsurmod = 1;

		uint8_t buf[64];
		size_t used = build_ac3_header(buf, sizeof(buf), &want);

		struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
		CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
		CHECK(got.bsi.acmod == acmod);
		CHECK(got.bsi.acmod_nfchans == expected_nfchans[acmod]);

		if ((acmod & 0x1) && (acmod != 0x1)) {
			CHECK(got.bsi.cmixlev == 1);
		} else {
			CHECK(got.bsi.cmixlev == 0); /* parser's own default when not present */
		}
		if (acmod & 0x4) {
			CHECK(got.bsi.surmixlev == 1);
		} else {
			CHECK(got.bsi.surmixlev == 0);
		}
		if (acmod == 0x2) {
			CHECK(got.bsi.dsurmod == 1);
		} else {
			CHECK(got.bsi.dsurmod == 0);
		}
	}
}

static void test_parse_dialnorm_is_negated(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 2;
	want.bsi.dialnorm = -17;

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
	CHECK(got.bsi.dialnorm == -17);
}

static void test_parse_optional_single_bit_gated_fields(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 2;
	want.bsi.lfeon = 1;
	want.bsi.compre = 1;
	want.bsi.compr = 0xAB;
	want.bsi.langcode = 1;
	want.bsi.langcod = 0xCD;
	want.bsi.audprodie = 1;
	want.bsi.mixlevel = 19;
	want.bsi.roomtyp = 2;
	want.bsi.copyrightb = 1;
	want.bsi.origbs = 1;

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);

	CHECK(got.bsi.lfeon == 1);
	CHECK(got.bsi.compre == 1);
	CHECK(got.bsi.compr == 0xAB);
	CHECK(got.bsi.langcode == 1);
	CHECK(got.bsi.langcod == 0xCD);
	CHECK(got.bsi.audprodie == 1);
	CHECK(got.bsi.mixlevel == 19);
	CHECK(got.bsi.roomtyp == 2);
	CHECK(got.bsi.copyrightb == 1);
	CHECK(got.bsi.origbs == 1);
}

static void test_parse_optional_fields_absent_when_flags_clear(void)
{
	/* Same fields as above, but with every enable-bit left at 0: the
	 * gated values must never be read from the bitstream (default 0
	 * from the parser, not whatever garbage the builder would have
	 * written into that struct position had it been serialized). */
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 2;
	want.bsi.compr = 0xFF;   /* must be ignored: compre == 0 */
	want.bsi.langcod = 0xFF; /* must be ignored: langcode == 0 */
	want.bsi.mixlevel = 31;  /* must be ignored: audprodie == 0 */
	want.bsi.roomtyp = 3;

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);

	CHECK(got.bsi.compre == 0);
	CHECK(got.bsi.compr == 0);
	CHECK(got.bsi.langcode == 0);
	CHECK(got.bsi.langcod == 0);
	CHECK(got.bsi.audprodie == 0);
	CHECK(got.bsi.mixlevel == 0);
	CHECK(got.bsi.roomtyp == 0);
}

static void test_parse_acmod_zero_dual_mono_second_channel(void)
{
	/* acmod == 0 is the 1+1 "dual mono" mode with a full second set of
	 * dialnorm2/compr2/langcod2/mixlevel2 fields. */
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 0;
	want.bsi.dialnorm2 = -9;
	want.bsi.compr2e = 1;
	want.bsi.compr2 = 0x11;
	want.bsi.langcod2e = 1;
	want.bsi.langcod2 = 0x22;
	want.bsi.audprodi2e = 1;
	want.bsi.mixlevel2 = 7;
	want.bsi.roomtyp2 = 1;

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
	CHECK(got.bsi.acmod_nfchans == 2); /* Ch1, Ch2 */
	CHECK(got.bsi.dialnorm2 == -9);
	CHECK(got.bsi.compr2e == 1);
	CHECK(got.bsi.compr2 == 0x11);
	CHECK(got.bsi.langcod2e == 1);
	CHECK(got.bsi.langcod2 == 0x22);
	CHECK(got.bsi.audprodi2e == 1);
	CHECK(got.bsi.mixlevel2 == 7);
	CHECK(got.bsi.roomtyp2 == 1);
}

static void test_parse_bsid8_timecodes(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.bsid = 8;
	want.bsi.acmod = 2;
	want.bsi.timecod1e = 1;
	want.bsi.timecod1 = 0x1234 & 0x3fff;
	want.bsi.timecod2e = 1;
	want.bsi.timecod2 = 0x2AAA & 0x3fff;

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
	CHECK(got.bsi.bsid == 8);
	CHECK(got.bsi.timecod1e == 1);
	CHECK(got.bsi.timecod1 == want.bsi.timecod1);
	CHECK(got.bsi.timecod2e == 1);
	CHECK(got.bsi.timecod2 == want.bsi.timecod2);
}

static void test_parse_bsid6_extended_bsi(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.bsid = 6;
	want.bsi.acmod = 2;
	want.bsi.xbsi1e = 1;
	want.bsi.dmixmod = 2;
	want.bsi.ltrtcmixlev = 3;
	want.bsi.ltrtsurmixlev = 4;
	want.bsi.lorocmixlev = 5;
	want.bsi.lorosurmixlev = 6;
	want.bsi.xbsi2e = 1;
	want.bsi.dsurexmod = 1;
	want.bsi.dheadphonmod = 2;
	want.bsi.adconvtyp = 1;
	want.bsi.xbsi2 = 0x5A;
	want.bsi.encinfo = 1;

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
	CHECK(got.bsi.bsid == 6);
	CHECK(got.bsi.xbsi1e == 1);
	CHECK(got.bsi.dmixmod == 2);
	CHECK(got.bsi.ltrtcmixlev == 3);
	CHECK(got.bsi.ltrtsurmixlev == 4);
	CHECK(got.bsi.lorocmixlev == 5);
	CHECK(got.bsi.lorosurmixlev == 6);
	CHECK(got.bsi.xbsi2e == 1);
	CHECK(got.bsi.dsurexmod == 1);
	CHECK(got.bsi.dheadphonmod == 2);
	CHECK(got.bsi.adconvtyp == 1);
	CHECK(got.bsi.xbsi2 == 0x5A);
	CHECK(got.bsi.encinfo == 1);
}

static void test_parse_bsid_other_skips_extended_and_timecode_sections(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.bsid = 1; /* neither 6 nor 8 */
	want.bsi.acmod = 2;
	want.bsi.addbsie = 1;
	want.bsi.addbsil = 2;
	want.bsi.addbsi[0] = 0xDE;
	want.bsi.addbsi[1] = 0xAD;
	want.bsi.addbsi[2] = 0xBE;

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
	CHECK(got.bsi.timecod1e == 0);
	CHECK(got.bsi.xbsi1e == 0);
	CHECK(got.bsi.addbsie == 1);
	CHECK(got.bsi.addbsil == 2);
	CHECK(got.bsi.addbsi[0] == 0xDE);
	CHECK(got.bsi.addbsi[1] == 0xAD);
	CHECK(got.bsi.addbsi[2] == 0xBE);
}

static void test_parse_addbsi_max_length(void)
{
	/* addbsil is 6 bits (0-63), meaning up to 64 bytes -- exactly the
	 * capacity of struct ltn_ac3_header_bsi_s's addbsi[64] array. */
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 2;
	want.bsi.addbsie = 1;
	want.bsi.addbsil = 63;
	for (int i = 0; i < 64; i++) {
		want.bsi.addbsi[i] = (uint8_t)i;
	}

	uint8_t buf[200];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
	CHECK(got.bsi.addbsil == 63);
	for (int i = 0; i < 64; i++) {
		CHECK(got.bsi.addbsi[i] == (uint8_t)i);
	}
}

static void test_parse_audioblk_dynrng(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 3; /* 3/0, nfchans == 3 */
	want.audioblk[0].blksw[0] = 1;
	want.audioblk[0].dithflag[2] = 1;
	want.audioblk[0].dynrnge = 1;
	want.audioblk[0].dynrng = 0x77;
	want.audioblk[5].dynrnge = 0; /* explicit: last block has no dynrng */

	uint8_t buf[64];
	size_t used = build_ac3_header(buf, sizeof(buf), &want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	CHECK(ltntstools_ac3_header_parse(&got, buf, (int)used) == 0);
	CHECK(got.audioblk[0].blksw[0] == 1);
	CHECK(got.audioblk[0].dithflag[2] == 1);
	CHECK(got.audioblk[0].dynrnge == 1);
	CHECK(got.audioblk[0].dynrng == 0x77);
	CHECK(got.audioblk[5].dynrnge == 0);
}

/* -------- dprintf(): smoke tests across the major branches -------- */

static void run_dprintf_smoke(const struct ltn_ac3_header_syncframe_s *want, const char *label)
{
	uint8_t buf[200];
	size_t used = build_ac3_header(buf, sizeof(buf), want);

	struct ltn_ac3_header_syncframe_s got;
	memset(&got, 0, sizeof(got));
	if (ltntstools_ac3_header_parse(&got, buf, (int)used) != 0) {
		fprintf(stderr, "FAIL: %s: parse failed for dprintf smoke case '%s'\n", __FILE__, label);
		g_failures++;
		return;
	}

	FILE *tmp = tmpfile();
	CHECK(tmp != NULL);
	if (!tmp) {
		return;
	}
	ltntstools_ac3_header_dprintf(fileno(tmp), &got);

	long len = ftell(tmp);
	CHECK(len > 0); /* must have produced some output for every branch */

	fclose(tmp);
}

static void test_dprintf_smoke_bsid8(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.bsid = 8;
	want.bsi.acmod = 7; /* 3/2, exercises cmixlev + surmixlev + full channel loops */
	want.bsi.lfeon = 1;
	want.bsi.compre = 1;
	want.bsi.langcode = 1;
	want.bsi.audprodie = 1;
	want.bsi.timecod1e = 1;
	want.bsi.timecod2e = 1;
	want.bsi.addbsie = 1;
	want.bsi.addbsil = 1;
	run_dprintf_smoke(&want, "bsid8/acmod7");
}

static void test_dprintf_smoke_bsid6(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.bsid = 6;
	want.bsi.acmod = 0; /* 1+1, exercises the dual-mono branch too */
	want.bsi.xbsi1e = 1;
	want.bsi.xbsi2e = 1;
	run_dprintf_smoke(&want, "bsid6/acmod0");
}

static void test_dprintf_smoke_acmod2_dsurmod(void)
{
	struct ltn_ac3_header_syncframe_s want;
	memset(&want, 0, sizeof(want));
	want.syncinfo.syncword = 0x0b77;
	want.bsi.acmod = 2; /* 2/0, exercises the dsurmod branch */
	run_dprintf_smoke(&want, "acmod2");
}

int main(void)
{
	test_parse_null_args();
	test_parse_rejects_short_buffer();
	test_parse_reports_failure_on_truncated_extended_bsi();

	test_parse_basic_syncinfo();
	test_parse_acmod_channel_count_table();
	test_parse_dialnorm_is_negated();
	test_parse_optional_single_bit_gated_fields();
	test_parse_optional_fields_absent_when_flags_clear();
	test_parse_acmod_zero_dual_mono_second_channel();
	test_parse_bsid8_timecodes();
	test_parse_bsid6_extended_bsi();
	test_parse_bsid_other_skips_extended_and_timecode_sections();
	test_parse_addbsi_max_length();
	test_parse_audioblk_dynrng();

	test_dprintf_smoke_bsid8();
	test_dprintf_smoke_bsid6();
	test_dprintf_smoke_acmod2_dsurmod();

	if (g_failures == 0) {
		printf("PASS: all ac3 tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d ac3 test(s) failed\n", g_failures);
	return 1;
}
