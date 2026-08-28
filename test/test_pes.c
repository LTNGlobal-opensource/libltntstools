/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/pes.c / src/libltntstools/pes.h.
 * Builds against ../src/pes.c plus ../src/crc32.c (needed only by
 * ltn_pes_packet_save_es/save_pes, which call ltntstools_getCRC32()).
 * klbitstream_readwriter.h and hexdump.h are header-only.
 *
 * ltn_pes_packet_pack() previously had 5 confirmed bugs, found while writing
 * the round-trip tests below (comparing pack()'s claimed `bits` return value
 * against klbs_get_byte_count(), the bitstream's real byte count) and now
 * fixed in src/pes.c:
 *
 * 1. Padding_stream (stream_id 0xBE): the "raw data" else-if branch that
 *    pack() uses for stream IDs without an extended header explicitly listed
 *    {0xBF,0xF0,0xF1,0xFF,0xF2,0xF8} but omitted 0xBE, so the nested
 *    `else if (pkt->stream_id == 0xBE) { ...write padding... }` inside that
 *    branch was unreachable dead code -- pack() wrote only the 6-byte prefix
 *    and nothing else. Fixed by giving 0xBE its own top-level branch,
 *    mirroring ltn_pes_packet_parse(), which already had one.
 *
 * 2. ESCR_flag: pack() wrote only 40 bits for the ESCR field but credited
 *    itself 48 bits. ESCR is a 48-bit field per ISO13818-1 (matches what
 *    parse() reads). Fixed to write 48 bits.
 *
 * 3. program_packet_sequence_counter_flag and PSTD_buffer_flag: both were
 *    marked "Not supported" and wrote NOTHING, yet both credited themselves
 *    16 bits. parse() DOES read real bits for both when set. Fixed:
 *    sequence_counter now writes a 16-bit placeholder (parse() discards the
 *    value anyway); PSTD_buffer now writes the real
 *    reserved('01')+scale+size fields, round-tripping losslessly.
 *
 * 4. pack_header_field_flag: also "Not supported", wrote nothing but (unlike
 *    #3) credited 0 bits too -- so it didn't shift anything itself, but
 *    parse() still reads a length byte + that many data bytes when the flag
 *    is set, silently consuming real payload as if it were header data.
 *    Fixed by writing a zero-length field (parse() never captured the
 *    original content in the first place, so there's nothing to preserve).
 *
 * 5. PES_extension_flag_2: pack() credited 8 bits for the marker+field
 *    length byte but never wrote it -- only the subsequent
 *    PES_extension_field_length data bytes were written. This one was only
 *    discovered *after* fixing #3, since bug #3's corruption had been
 *    masking it in the original failing test. Fixed to write the
 *    marker+length byte.
 *
 * The "NULL argument safety" section below covers a separate fix: every
 * pes.c function used to dereference its pointer argument(s) unconditionally,
 * so a NULL caller mistake crashed the process instead of returning an
 * error/no-op. Guards were added to init/free/pack/parse/dump/copy/clone/
 * is_audio/is_video/has_PTS/has_DTS; writer_init/save_es/save_pes already
 * guarded correctly.
 *
 * Four more fixes, each with its own test below:
 *
 * 6. ltn_pes_packet_parse() used to store a parsed ES_rate value back into
 *    `pkt->ES_rate_flag` itself (reusing the flag field as the value field)
 *    instead of the dedicated `pkt->ES_rate` struct field, and
 *    ltn_pes_packet_pack() mirrored that same reuse -- so pack<->parse round
 *    trips were internally consistent, but ltn_pes_packet_dump()'s
 *    `DISPLAY_U32(i, pkt->ES_rate)` line always displayed 0. Fixed: both now
 *    use pkt->ES_rate, leaving pkt->ES_rate_flag a clean boolean.
 *
 * 7. ltn_pes_packet_pack()'s "raw data" branch (special stream IDs
 *    BF/F0/F1/FF/F2/F8, no extended header) wrote pkt->PES_packet_length
 *    bytes from pkt->data, while the normal branch used pkt->dataLengthBytes
 *    -- the actual size of the pkt->data allocation. A caller who set
 *    PES_packet_length larger than the real buffer (e.g. by copying a
 *    packet and only updating one field) caused an out-of-bounds read.
 *    Fixed: this branch now writes min(dataLengthBytes, PES_packet_length)
 *    bytes.
 *
 * 8. ltn_pes_packet_copy()'s struct-wide memcpy() copies dataLengthBytes/
 *    rawBufferLengthBytes verbatim, but dst->data/dst->rawBuffer are only
 *    repopulated when src->data/src->rawBuffer is non-NULL -- so a src with
 *    a NULL data pointer but nonzero dataLengthBytes produced a dst with
 *    the same NULL-pointer/nonzero-length mismatch. Fixed: dst's length
 *    field is now zeroed whenever the matching src pointer is NULL.
 *
 * 9. ltn_pes_packet_save_es()/save_pes() passed pes->data/pes->rawBuffer to
 *    getCRC32()/fwrite() without checking they were non-NULL first. Fixed:
 *    both now skip the CRC/fwrite calls when the length is 0, and refuse to
 *    write a file at all (return < 0) when the pointer is NULL but the
 *    length claims otherwise.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "libltntstools/pes.h"
#include "libltntstools/klbitstream_readwriter.h"
#include "libltntstools/crc32.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	struct ltn_pes_packet_s *pkt = ltn_pes_packet_alloc();
	CHECK(pkt != NULL);
	CHECK(pkt->data == NULL);
	CHECK(pkt->dataLengthBytes == 0);
	ltn_pes_packet_free(pkt);
}

static void test_init_resets_and_frees_existing_data(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.data = malloc(4);
	pkt.dataLengthBytes = 4;
	pkt.rawBuffer = malloc(8);
	pkt.rawBufferLengthBytes = 8;
	pkt.stream_id = 0xE0;

	ltn_pes_packet_init(&pkt);

	CHECK(pkt.data == NULL);
	CHECK(pkt.dataLengthBytes == 0);
	CHECK(pkt.rawBuffer == NULL);
	CHECK(pkt.stream_id == 0);
}

static void test_copy_deep_copies_data_and_rawbuffer(void)
{
	struct ltn_pes_packet_s src = { 0 };
	src.stream_id = 0xE0;
	src.PTS = 12345;
	uint8_t d[3] = { 1, 2, 3 };
	uint8_t r[5] = { 9, 8, 7, 6, 5 };
	src.data = d;
	src.dataLengthBytes = sizeof(d);
	src.rawBuffer = r;
	src.rawBufferLengthBytes = sizeof(r);

	struct ltn_pes_packet_s dst = { 0 };
	ltn_pes_packet_copy(&dst, &src);

	CHECK(dst.stream_id == 0xE0);
	CHECK(dst.PTS == 12345);
	CHECK(dst.data != src.data); /* deep copy, not aliased */
	CHECK(memcmp(dst.data, d, sizeof(d)) == 0);
	CHECK(dst.rawBuffer != src.rawBuffer);
	CHECK(memcmp(dst.rawBuffer, r, sizeof(r)) == 0);

	free(dst.data);
	free(dst.rawBuffer);
}

/* Issue #8 (see file header): a src with a NULL data/rawBuffer pointer but
 * a nonzero length used to leave dst with the same inconsistent
 * pointer/length pair (copied verbatim by the struct-wide memcpy()). */
static void test_copy_zeroes_length_when_src_pointer_is_null(void)
{
	struct ltn_pes_packet_s src = { 0 };
	src.stream_id = 0xE0;
	src.data = NULL;
	src.dataLengthBytes = 99; /* inconsistent on purpose */
	src.rawBuffer = NULL;
	src.rawBufferLengthBytes = 77; /* inconsistent on purpose */

	struct ltn_pes_packet_s dst = { 0 };
	ltn_pes_packet_copy(&dst, &src);

	CHECK(dst.data == NULL);
	CHECK(dst.dataLengthBytes == 0);
	CHECK(dst.rawBuffer == NULL);
	CHECK(dst.rawBufferLengthBytes == 0);
}

static void test_clone_independent_of_source(void)
{
	struct ltn_pes_packet_s *src = ltn_pes_packet_alloc();
	src->stream_id = 0xC0;
	src->data = malloc(2);
	src->data[0] = 0xAA;
	src->data[1] = 0xBB;
	src->dataLengthBytes = 2;

	struct ltn_pes_packet_s *clone = ltn_pes_packet_clone(src);
	CHECK(clone != NULL);
	CHECK(clone->data != src->data);
	CHECK(memcmp(clone->data, src->data, 2) == 0);

	/* Mutate the source's payload after cloning; the clone must be unaffected. */
	src->data[0] = 0xFF;
	CHECK(clone->data[0] == 0xAA);

	ltn_pes_packet_free(src);
	ltn_pes_packet_free(clone);
}

/* -------- NULL argument safety --------
 * Every function below used to dereference its pointer argument(s)
 * unconditionally; a NULL caller mistake crashed the process. Each of these
 * exercises the guard added for that function -- the only thing under test
 * is that the call returns the documented "did nothing" value (or, for void
 * functions, simply returns) instead of segfaulting, since a missed guard
 * would crash this whole test binary rather than fail a single CHECK().
 */

static void test_init_and_free_accept_null(void)
{
	ltn_pes_packet_init(NULL);
	ltn_pes_packet_free(NULL);
	CHECK(1); /* reaching here without crashing is the point of this test */
}

static void test_pack_rejects_null_args(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.stream_id = 0xE0;

	uint8_t buf[16];
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_write_set_buffer(&bs, buf, sizeof(buf));

	CHECK(ltn_pes_packet_pack(NULL, &bs) == 0);
	CHECK(ltn_pes_packet_pack(&pkt, NULL) == 0);
}

static void test_parse_rejects_null_args(void)
{
	uint8_t buf[16] = { 0 };
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, sizeof(buf));

	struct ltn_pes_packet_s pkt = { 0 };

	CHECK(ltn_pes_packet_parse(NULL, &bs, 0) == 0);
	CHECK(ltn_pes_packet_parse(&pkt, NULL, 0) == 0);
}

static void test_dump_accepts_null_pkt(void)
{
	ltn_pes_packet_dump(NULL, "  ");
	ltn_pes_packet_dump_with_options(NULL, "  ", 0xffffffff);
	CHECK(1);
}

static void test_dump_accepts_null_indent(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.stream_id = 0xE0;

	ltn_pes_packet_dump(&pkt, NULL);
	ltn_pes_packet_dump_with_options(&pkt, NULL, 0x01);
	CHECK(1);
}

static void test_copy_rejects_null_args(void)
{
	struct ltn_pes_packet_s src = { 0 };
	src.stream_id = 0xE0;

	struct ltn_pes_packet_s dst = { 0 };
	dst.stream_id = 0xAA;

	ltn_pes_packet_copy(NULL, &src);
	ltn_pes_packet_copy(&dst, NULL);

	/* Neither call should have touched dst. */
	CHECK(dst.stream_id == 0xAA);
	CHECK(dst.data == NULL);
}

static void test_clone_rejects_null_src(void)
{
	struct ltn_pes_packet_s *clone = ltn_pes_packet_clone(NULL);
	CHECK(clone == NULL);
}

static void test_is_audio_null_returns_false(void)
{
	CHECK(ltn_pes_packet_is_audio(NULL) == 0);
}

static void test_is_video_null_returns_false(void)
{
	CHECK(ltn_pes_packet_is_video(NULL) == 0);
}

static void test_has_pts_null_returns_false(void)
{
	CHECK(ltn_pes_packet_has_PTS(NULL) == 0);
}

static void test_has_dts_null_returns_false(void)
{
	CHECK(ltn_pes_packet_has_DTS(NULL) == 0);
}

/* -------- stream_id classification -------- */

static void test_is_audio_stream_id_ranges(void)
{
	struct ltn_pes_packet_s pes = { 0 };

	pes.stream_id = 0xC0; CHECK(ltn_pes_packet_is_audio(&pes) == 1);
	pes.stream_id = 0xDF; CHECK(ltn_pes_packet_is_audio(&pes) == 1);
	pes.stream_id = 0xBD; CHECK(ltn_pes_packet_is_audio(&pes) == 1); /* AC3/private */
	pes.stream_id = 0xFD; CHECK(ltn_pes_packet_is_audio(&pes) == 1);
	pes.stream_id = 0xE0; CHECK(ltn_pes_packet_is_audio(&pes) == 0); /* video range */
	pes.stream_id = 0xBC; CHECK(ltn_pes_packet_is_audio(&pes) == 0);
}

static void test_is_video_stream_id_ranges(void)
{
	struct ltn_pes_packet_s pes = { 0 };

	pes.stream_id = 0xE0; CHECK(ltn_pes_packet_is_video(&pes) == 1);
	pes.stream_id = 0xEF; CHECK(ltn_pes_packet_is_video(&pes) == 1);
	pes.stream_id = 0xC0; CHECK(ltn_pes_packet_is_video(&pes) == 0); /* audio range */
	pes.stream_id = 0xF0; CHECK(ltn_pes_packet_is_video(&pes) == 0);
}

static void test_has_pts_dts_flag_combinations(void)
{
	struct ltn_pes_packet_s pes = { 0 };

	pes.PTS_DTS_flags = 0; CHECK(ltn_pes_packet_has_PTS(&pes) == 0); CHECK(ltn_pes_packet_has_DTS(&pes) == 0);
	pes.PTS_DTS_flags = 1; CHECK(ltn_pes_packet_has_PTS(&pes) == 0); CHECK(ltn_pes_packet_has_DTS(&pes) == 1);
	pes.PTS_DTS_flags = 2; CHECK(ltn_pes_packet_has_PTS(&pes) == 1); CHECK(ltn_pes_packet_has_DTS(&pes) == 0);
	pes.PTS_DTS_flags = 3; CHECK(ltn_pes_packet_has_PTS(&pes) == 1); CHECK(ltn_pes_packet_has_DTS(&pes) == 1);
}

/* -------- parse(): hand-constructed, independently verified byte KATs --------
 * Bytes below were generated with a standalone Python bit-packer that
 * mirrors the ISO13818-1 PES header layout (not by hand), then cross
 * checked against src/pes.c's own field ordering before being pasted here.
 * stream_id=0xE0 (video), PTS_DTS_flags=2 (PTS only), PTS=5000000001,
 * 4-byte payload {0xDE,0xAD,0xBE,0xEF}.
 */
static const uint8_t kat_pts_only[] = {
	0x00, 0x00, 0x01, 0xe0, 0x00, 0x0c, 0x80, 0x80, 0x05, 0x29, 0xa8, 0x17, 0xe4, 0x03, 0xde, 0xad, 0xbe, 0xef
};

/* Same as above but with the final PTS marker bit flipped 1 -> 0 (byte 13: 0x03 -> 0x02). */
static const uint8_t kat_pts_bad_marker[] = {
	0x00, 0x00, 0x01, 0xe0, 0x00, 0x0c, 0x80, 0x80, 0x05, 0x29, 0xa8, 0x17, 0xe4, 0x02, 0xde, 0xad, 0xbe, 0xef
};

static void test_parse_known_good_pts_only_header(void)
{
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, (uint8_t *)kat_pts_only, sizeof(kat_pts_only));

	struct ltn_pes_packet_s pkt = { 0 };
	ssize_t bits = ltn_pes_packet_parse(&pkt, &bs, 0 /* don't skip data */);

	CHECK(bits == (ssize_t)sizeof(kat_pts_only) * 8);
	CHECK(pkt.packet_start_code_prefix == 0x000001);
	CHECK(pkt.stream_id == 0xE0);
	CHECK(pkt.PES_packet_length == 12);
	CHECK(pkt.PTS_DTS_flags == 2);
	CHECK(pkt.PTS == 5000000001LL);
	CHECK(ltn_pes_packet_has_PTS(&pkt) == 1);
	CHECK(ltn_pes_packet_has_DTS(&pkt) == 0);
	CHECK(pkt.dataLengthBytes == 4);
	CHECK(pkt.data != NULL);
	if (pkt.data) {
		uint8_t expect[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
		CHECK(memcmp(pkt.data, expect, 4) == 0);
	}

	if (pkt.data) free(pkt.data);
}

static void test_parse_corrupted_pts_marker_bit_yields_negative_one_pts(void)
{
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, (uint8_t *)kat_pts_bad_marker, sizeof(kat_pts_bad_marker));

	struct ltn_pes_packet_s pkt = { 0 };
	ltn_pes_packet_parse(&pkt, &bs, 0);

	/* A bad marker bit poisons PTS to -1, but parsing still proceeds and the
	 * bitstream cursor still advances the full 40 bits for the timestamp,
	 * so the payload is still correctly located and extracted. */
	CHECK(pkt.PTS == -1);
	CHECK(pkt.dataLengthBytes == 4);
	if (pkt.data) {
		uint8_t expect[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
		CHECK(memcmp(pkt.data, expect, 4) == 0);
		free(pkt.data);
	}
}

static void test_parse_too_short_buffer_returns_zero_bits(void)
{
	uint8_t buf[4] = { 0 }; /* < 8 bytes free */
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, sizeof(buf));

	struct ltn_pes_packet_s pkt = { 0 };
	ssize_t bits = ltn_pes_packet_parse(&pkt, &bs, 0);
	CHECK(bits == 0);
}

static void test_parse_clamps_length_to_available_bytes(void)
{
	/* Same header as kat_pts_only, but PES_packet_length field lies (says
	 * 200), while the buffer is truncated right after the header, with only
	 * 1 payload byte actually available. */
	uint8_t buf[] = {
		0x00, 0x00, 0x01, 0xe0, 0x00, 200, 0x80, 0x80, 0x05, 0x29, 0xa8, 0x17, 0xe4, 0x03, 0xAB
	};
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, sizeof(buf));

	struct ltn_pes_packet_s pkt = { 0 };
	ltn_pes_packet_parse(&pkt, &bs, 0);

	/* byte_count_free at the point PES_packet_length is read == sizeof(buf) - 6 */
	CHECK(pkt.PES_packet_length == (uint32_t)(sizeof(buf) - 6));
	CHECK(pkt.dataLengthBytes == 1);
	if (pkt.data) {
		CHECK(pkt.data[0] == 0xAB);
		free(pkt.data);
	}
}

static void test_parse_skip_data_true_omits_payload(void)
{
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, (uint8_t *)kat_pts_only, sizeof(kat_pts_only));

	struct ltn_pes_packet_s pkt = { 0 };
	ltn_pes_packet_parse(&pkt, &bs, 1 /* skipData */);

	CHECK(pkt.skipPayloadParsing == 1);
	CHECK(pkt.data == NULL);
	CHECK(pkt.dataLengthBytes == 0);
	CHECK(pkt.PTS == 5000000001LL); /* header fields are still parsed */
}

/* -------- pack() -> parse() round trips --------
 * pack() writes whatever PES_packet_length is already in the struct (it
 * does not compute it), so each helper below packs once to discover the
 * real byte count from pack()'s own return value, patches the
 * PES_packet_length field bytes directly in the buffer to match, and only
 * then treats the buffer as parseable -- this avoids hand-deriving the
 * expected byte length per flag combination for every test.
 */
static ssize_t pack_with_correct_length(struct ltn_pes_packet_s *pkt, uint8_t *buf, int bufLen)
{
	memset(buf, 0, bufLen);
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_write_set_buffer(&bs, buf, bufLen);

	ssize_t bits = ltn_pes_packet_pack(pkt, &bs);
	int totalBytes = (int)(bits / 8);
	uint16_t correctLength = (uint16_t)(totalBytes - 6); /* per-spec: bytes after the length field itself */

	buf[4] = (correctLength >> 8) & 0xff;
	buf[5] = correctLength & 0xff;

	return totalBytes;
}

static void test_roundtrip_no_optional_fields(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.data_alignment_indicator = 1;
	uint8_t payload[3] = { 0x01, 0x02, 0x03 };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.stream_id == 0xE0);
	CHECK(parsed.data_alignment_indicator == 1);
	CHECK(parsed.PTS_DTS_flags == 0);
	CHECK(parsed.dataLengthBytes == 3);
	CHECK(parsed.data && memcmp(parsed.data, payload, 3) == 0);

	if (parsed.data) free(parsed.data);
}

static void test_roundtrip_pts_only(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xC0;
	pkt.PTS_DTS_flags = 2;
	pkt.PTS = 8589934590LL; /* near the 33-bit max */
	uint8_t payload[2] = { 0xAA, 0xBB };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.PTS_DTS_flags == 2);
	CHECK(parsed.PTS == pkt.PTS);
	CHECK(ltn_pes_packet_has_PTS(&parsed) == 1);
	CHECK(ltn_pes_packet_has_DTS(&parsed) == 0);
	CHECK(parsed.dataLengthBytes == 2);
	CHECK(parsed.data && memcmp(parsed.data, payload, 2) == 0);

	if (parsed.data) free(parsed.data);
}

static void test_roundtrip_pts_and_dts(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.PTS_DTS_flags = 3;
	pkt.PTS = 900000;
	pkt.DTS = 810000;
	uint8_t payload[1] = { 0x7E };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.PTS == 900000);
	CHECK(parsed.DTS == 810000);
	CHECK(ltn_pes_packet_has_PTS(&parsed) == 1);
	CHECK(ltn_pes_packet_has_DTS(&parsed) == 1);

	if (parsed.data) free(parsed.data);
}

/* Issue #6 (see file header): ES_rate must round-trip through the dedicated
 * pkt->ES_rate field, and pkt->ES_rate_flag must come back as a clean
 * boolean (not the 22-bit rate value it used to be overwritten with). */
static void test_roundtrip_es_rate(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.ES_rate_flag = 1;
	pkt.ES_rate = 0x3ABCD; /* 22 bits */
	uint8_t payload[1] = { 0x5A };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.ES_rate_flag == 1);
	CHECK(parsed.ES_rate == 0x3ABCD);
	CHECK(parsed.dataLengthBytes == 1);
	CHECK(parsed.data && parsed.data[0] == 0x5A);

	if (parsed.data) free(parsed.data);
}

/* KNOWN BUG: see file header (#2). pack() under-writes the ESCR field by
 * 1 byte relative to what it claims and what parse() expects, so the
 * payload comes out shifted/corrupted. */
static void test_roundtrip_escr_flag(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.ESCR_flag = 1;
	uint8_t payload[2] = { 0x11, 0x22 };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.ESCR_flag == 1);
	CHECK(parsed.dataLengthBytes == 2);
	CHECK(parsed.data && memcmp(parsed.data, payload, 2) == 0);

	if (parsed.data) free(parsed.data);
}

static void test_roundtrip_trick_mode_and_copy_info_flags(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.DSM_trick_mode_flag = 1;
	pkt.additional_copy_info_flag = 1;
	pkt.additional_copy_info = 0x55; /* 7 bits, top bit of the field is a marker pack always sets to 1 */
	uint8_t payload[1] = { 0x99 };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.DSM_trick_mode_flag == 1);
	CHECK(parsed.additional_copy_info_flag == 1);
	CHECK(parsed.additional_copy_info == (0x55 & 0x7f));
	CHECK(parsed.dataLengthBytes == 1);

	if (parsed.data) free(parsed.data);
}

static void test_roundtrip_crc_flag(void)
{
	/* pack() always writes 0 for the CRC value itself ("Not supported"),
	 * so this only exercises the flag/field length bookkeeping, not a real
	 * CRC value round trip. */
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.PES_CRC_flag = 1;
	uint8_t payload[1] = { 0x01 };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.PES_CRC_flag == 1);
	CHECK(parsed.previous_PES_packet_CRC == 0);
	CHECK(parsed.dataLengthBytes == 1);

	if (parsed.data) free(parsed.data);
}

static void test_roundtrip_extension_private_data(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.PES_extension_flag = 1;
	pkt.PES_private_data_flag = 1;
	uint8_t payload[2] = { 0x44, 0x55 };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.PES_extension_flag == 1);
	CHECK(parsed.PES_private_data_flag == 1);
	CHECK(parsed.dataLengthBytes == 2);
	CHECK(parsed.data && memcmp(parsed.data, payload, 2) == 0);

	if (parsed.data) free(parsed.data);
}

/* KNOWN BUG: see file header (#3). program_packet_sequence_counter_flag and
 * PSTD_buffer_flag each claim 16 bits written but write 0, shifting
 * everything packed afterward. */
static void test_roundtrip_extension_seqcounter_pstd_ext2(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xE0;
	pkt.PES_extension_flag = 1;
	pkt.program_packet_sequence_counter_flag = 1;
	pkt.PSTD_buffer_flag = 1;
	pkt.PES_extension_flag_2 = 1;
	pkt.PES_extension_field_length = 3;
	uint8_t payload[1] = { 0x66 };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload);

	uint8_t buf[64];
	int totalBytes = (int)pack_with_correct_length(&pkt, buf, sizeof(buf));

	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_read_set_buffer(&bs, buf, totalBytes);

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &bs, 0);

	CHECK(parsed.program_packet_sequence_counter_flag == 1);
	CHECK(parsed.PSTD_buffer_flag == 1);
	CHECK(parsed.PES_extension_flag_2 == 1);
	CHECK(parsed.PES_extension_field_length == 3);
	CHECK(parsed.dataLengthBytes == 1);
	CHECK(parsed.data && parsed.data[0] == 0x66);

	if (parsed.data) free(parsed.data);
}

/* stream IDs BF/F0/F1/F2/F8/FF skip the extended header entirely: raw
 * pkt->data bytes, PES_packet_length long, are written/read directly. */
static void test_roundtrip_special_streamid_raw_data(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xF0; /* ECM */
	uint8_t payload[6] = { 1, 2, 3, 4, 5, 6 };
	pkt.data = payload;
	pkt.dataLengthBytes = sizeof(payload); /* must match the real pkt->data allocation, see issue #7 below */
	pkt.PES_packet_length = sizeof(payload); /* this branch writes min(dataLengthBytes, PES_packet_length) bytes */

	uint8_t buf[64];
	memset(buf, 0, sizeof(buf));
	struct klbs_context_s wbs;
	klbs_init(&wbs);
	klbs_write_set_buffer(&wbs, buf, sizeof(buf));
	ssize_t bits = ltn_pes_packet_pack(&pkt, &wbs);
	CHECK(bits == (ssize_t)sizeof(payload) * 8);

	struct klbs_context_s rbs;
	klbs_init(&rbs);
	klbs_read_set_buffer(&rbs, buf, 6 + sizeof(payload)); /* 6-byte prefix + raw payload */

	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &rbs, 0);

	CHECK(parsed.stream_id == 0xF0);
	CHECK(parsed.PES_packet_length == sizeof(payload));
	CHECK(parsed.data && memcmp(parsed.data, payload, sizeof(payload)) == 0);

	if (parsed.data) free(parsed.data);
}

/* Issue #7: pkt->data is malloc()'d to exactly dataLengthBytes (3), but
 * PES_packet_length lies and claims 6. Before the fix, pack() wrote
 * PES_packet_length bytes from pkt->data regardless of dataLengthBytes,
 * reading 3 bytes past the end of this allocation. Run under
 * ASan/valgrind to catch a regression; without one, the min() bound is
 * checked directly via the returned bit count and packed bytes below. */
static void test_pack_special_streamid_bounds_write_by_dataLengthBytes(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xF0; /* ECM */
	uint8_t *payload = malloc(3);
	payload[0] = 0xAA; payload[1] = 0xBB; payload[2] = 0xCC;
	pkt.data = payload;
	pkt.dataLengthBytes = 3;
	pkt.PES_packet_length = 6; /* lies: claims more than the real allocation */

	uint8_t buf[64];
	memset(buf, 0, sizeof(buf));
	struct klbs_context_s wbs;
	klbs_init(&wbs);
	klbs_write_set_buffer(&wbs, buf, sizeof(buf));
	ssize_t bits = ltn_pes_packet_pack(&pkt, &wbs);

	/* Only the 3 real bytes were written, not the claimed 6. */
	CHECK(bits == 3 * 8);
	CHECK(memcmp(buf + 6, payload, 3) == 0);

	free(payload);
}

static void test_pack_padding_stream(void)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = 0xBE; /* padding_stream */
	pkt.PES_packet_length = 4;
	pkt.data = NULL; /* NULL data means "write padding" */

	uint8_t buf[32];
	memset(buf, 0, sizeof(buf));
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_write_set_buffer(&bs, buf, sizeof(buf));

	ssize_t bits = ltn_pes_packet_pack(&pkt, &bs);

	/* This branch's `bits` return value convention (confirmed against
	 * ltn_pes_packet_parse(), which returns the same for the same input)
	 * does NOT include the 6-byte prefix, unlike the main/extended-header
	 * branch -- so the meaningful check is the real byte count and content. */
	CHECK(bits == 32); /* 4 padding bytes */
	CHECK(klbs_get_byte_count(&bs) == 10); /* 6-byte prefix + 4 padding bytes, actually written */

	uint8_t expect[10] = { 0x00, 0x00, 0x01, 0xBE, 0x00, 0x04, 0xff, 0xff, 0xff, 0xff };
	CHECK(memcmp(buf, expect, sizeof(expect)) == 0);

	/* Round trip: parse() must read back exactly what was packed. */
	struct klbs_context_s rbs;
	klbs_init(&rbs);
	klbs_read_set_buffer(&rbs, buf, 10);
	struct ltn_pes_packet_s parsed = { 0 };
	ltn_pes_packet_parse(&parsed, &rbs, 0);
	CHECK(parsed.stream_id == 0xBE);
	CHECK(parsed.PES_packet_length == 4);
}

/* -------- writer -------- */

static void test_writer_init_rejects_null_args(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	CHECK(ltn_pes_packet_writer_init(NULL, "/tmp") < 0);
	CHECK(ltn_pes_packet_writer_init(&ctx, NULL) < 0);
}

static void test_writer_init_copies_dirname(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	CHECK(ltn_pes_packet_writer_init(&ctx, "/tmp/somewhere") == 0);
	CHECK(strcmp(ctx.dirname, "/tmp/somewhere") == 0);
	CHECK(ctx.nr == 0);
}

static void test_save_es_rejects_null_args(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	ltn_pes_packet_writer_init(&ctx, "/tmp");
	struct ltn_pes_packet_s pes = { 0 };

	CHECK(ltn_pes_packet_save_es(NULL, &pes) < 0);
	CHECK(ltn_pes_packet_save_es(&ctx, NULL) < 0);
}

static void test_save_pes_rejects_null_args(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	ltn_pes_packet_writer_init(&ctx, "/tmp");
	struct ltn_pes_packet_s pes = { 0 };

	CHECK(ltn_pes_packet_save_pes(NULL, &pes) < 0);
	CHECK(ltn_pes_packet_save_pes(&ctx, NULL) < 0);
}

static const char *scratch_dir(void)
{
	const char *d = getenv("TEST_SCRATCH_DIR");
	return d ? d : "/tmp";
}

static void test_save_es_writes_file_with_correct_content(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	ltn_pes_packet_writer_init(&ctx, scratch_dir());

	struct ltn_pes_packet_s pes = { 0 };
	uint8_t data[5] = { 1, 2, 3, 4, 5 };
	pes.data = data;
	pes.dataLengthBytes = sizeof(data);
	pes.PTS = 111;
	pes.DTS = 222;

	uint32_t crc = 0;
	ltntstools_getCRC32(pes.data, pes.dataLengthBytes, &crc);

	char expectFn[600];
	snprintf(expectFn, sizeof(expectFn), "%s/es-seq%014u-pts%014u-dts%014u-len%08u-crc%08x",
		scratch_dir(), 0, 111, 222, (unsigned)sizeof(data), crc);

	CHECK(ltn_pes_packet_save_es(&ctx, &pes) == 0);
	CHECK(ctx.nr == 1);

	FILE *f = fopen(expectFn, "rb");
	CHECK(f != NULL);
	if (f) {
		uint8_t readback[5] = { 0 };
		CHECK(fread(readback, 1, sizeof(readback), f) == sizeof(readback));
		CHECK(memcmp(readback, data, sizeof(data)) == 0);
		fclose(f);
		remove(expectFn);
	}
}

static void test_save_pes_writes_file_with_correct_content(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	ltn_pes_packet_writer_init(&ctx, scratch_dir());

	struct ltn_pes_packet_s pes = { 0 };
	uint8_t raw[7] = { 9, 8, 7, 6, 5, 4, 3 };
	pes.rawBuffer = raw;
	pes.rawBufferLengthBytes = sizeof(raw);
	pes.dataLengthBytes = 0; /* embedded in filename, independent of rawBuffer length */
	pes.PTS = 333;
	pes.DTS = 444;

	uint32_t crc = 0;
	ltntstools_getCRC32(pes.rawBuffer, pes.rawBufferLengthBytes, &crc);

	char expectFn[600];
	snprintf(expectFn, sizeof(expectFn), "%s/pes-seq%014u-pts%014u-dts%014u-len%08u-crc%08x",
		scratch_dir(), 0, 333, 444, (unsigned)pes.dataLengthBytes, crc);

	CHECK(ltn_pes_packet_save_pes(&ctx, &pes) == 0);

	FILE *f = fopen(expectFn, "rb");
	CHECK(f != NULL);
	if (f) {
		uint8_t readback[7] = { 0 };
		CHECK(fread(readback, 1, sizeof(readback), f) == sizeof(readback));
		CHECK(memcmp(readback, raw, sizeof(raw)) == 0);
		fclose(f);
		remove(expectFn);
	}
}

/* Issue #9 (see file header): a NULL data/rawBuffer pointer paired with a
 * nonzero length is an inconsistent packet -- save_es()/save_pes() must
 * refuse to write bogus output rather than pass NULL+nonzero-count to
 * getCRC32()/fwrite(). */
static void test_save_es_rejects_inconsistent_null_data(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	ltn_pes_packet_writer_init(&ctx, scratch_dir());

	struct ltn_pes_packet_s pes = { 0 };
	pes.data = NULL;
	pes.dataLengthBytes = 5; /* inconsistent on purpose */

	CHECK(ltn_pes_packet_save_es(&ctx, &pes) < 0);
	CHECK(ctx.nr == 0); /* nothing written, sequence number not consumed */
}

static void test_save_pes_rejects_inconsistent_null_rawbuffer(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	ltn_pes_packet_writer_init(&ctx, scratch_dir());

	struct ltn_pes_packet_s pes = { 0 };
	pes.rawBuffer = NULL;
	pes.rawBufferLengthBytes = 5; /* inconsistent on purpose */

	CHECK(ltn_pes_packet_save_pes(&ctx, &pes) < 0);
	CHECK(ctx.nr == 0);
}

/* A NULL pointer with a genuinely zero length (an empty ES/PES) is valid --
 * this must still produce a 0-byte file, not crash. */
static void test_save_es_and_save_pes_handle_null_pointer_zero_length(void)
{
	struct ltn_pes_packet_writer_ctx ctx;
	ltn_pes_packet_writer_init(&ctx, scratch_dir());

	struct ltn_pes_packet_s pes = { 0 };
	pes.data = NULL;
	pes.dataLengthBytes = 0;
	pes.rawBuffer = NULL;
	pes.rawBufferLengthBytes = 0;

	char esFn[600], pesFn[600];
	snprintf(esFn, sizeof(esFn), "%s/es-seq%014u-pts%014u-dts%014u-len%08u-crc%08x",
		scratch_dir(), 0, 0, 0, 0, 0);
	snprintf(pesFn, sizeof(pesFn), "%s/pes-seq%014u-pts%014u-dts%014u-len%08u-crc%08x",
		scratch_dir(), 1, 0, 0, 0, 0);

	CHECK(ltn_pes_packet_save_es(&ctx, &pes) == 0);
	CHECK(ltn_pes_packet_save_pes(&ctx, &pes) == 0);
	CHECK(ctx.nr == 2);

	remove(esFn);
	remove(pesFn);
}

int main(void)
{
	test_alloc_free_basic();
	test_init_resets_and_frees_existing_data();
	test_copy_deep_copies_data_and_rawbuffer();
	test_copy_zeroes_length_when_src_pointer_is_null();
	test_clone_independent_of_source();

	test_init_and_free_accept_null();
	test_pack_rejects_null_args();
	test_parse_rejects_null_args();
	test_dump_accepts_null_pkt();
	test_dump_accepts_null_indent();
	test_copy_rejects_null_args();
	test_clone_rejects_null_src();
	test_is_audio_null_returns_false();
	test_is_video_null_returns_false();
	test_has_pts_null_returns_false();
	test_has_dts_null_returns_false();

	test_is_audio_stream_id_ranges();
	test_is_video_stream_id_ranges();
	test_has_pts_dts_flag_combinations();

	test_parse_known_good_pts_only_header();
	test_parse_corrupted_pts_marker_bit_yields_negative_one_pts();
	test_parse_too_short_buffer_returns_zero_bits();
	test_parse_clamps_length_to_available_bytes();
	test_parse_skip_data_true_omits_payload();

	test_roundtrip_no_optional_fields();
	test_roundtrip_pts_only();
	test_roundtrip_pts_and_dts();
	test_roundtrip_es_rate();
	test_roundtrip_escr_flag();
	test_roundtrip_trick_mode_and_copy_info_flags();
	test_roundtrip_crc_flag();
	test_roundtrip_extension_private_data();
	test_roundtrip_extension_seqcounter_pstd_ext2();
	test_roundtrip_special_streamid_raw_data();
	test_pack_special_streamid_bounds_write_by_dataLengthBytes();
	test_pack_padding_stream();

	test_writer_init_rejects_null_args();
	test_writer_init_copies_dirname();
	test_save_es_rejects_null_args();
	test_save_pes_rejects_null_args();
	test_save_es_rejects_inconsistent_null_data();
	test_save_pes_rejects_inconsistent_null_rawbuffer();
	test_save_es_and_save_pes_handle_null_pointer_zero_length();
	test_save_es_writes_file_with_correct_content();
	test_save_pes_writes_file_with_correct_content();

	if (g_failures == 0) {
		printf("PASS: all pes tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d pes test(s) failed\n", g_failures);
	return 1;
}
