/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/nal_h264.c / src/libltntstools/nal_h264.h.
 *
 * Several real bugs were found while writing these tests and have been
 * FIXED (in src/nal_h264.c, alongside its src/nal_h265.c sibling which
 * shared some of the same defects):
 *
 * 1. ltn_nal_h264_strip_emulation_prevention() used memcpy() to shift the
 *    buffer left by one byte after removing an emulation-prevention 0x03 --
 *    but source and destination overlap by all but one byte, which is
 *    undefined behavior for memcpy(). Confirmed via ASan's
 *    memcpy-param-overlap diagnostic (it happened to produce correct
 *    output on this platform only because this memcpy() implementation
 *    copies forward). Fixed by switching to memmove().
 *
 * 2. h264Nals_lookupName()/h264Nals_lookupType() indexed the sparse
 *    h264Nals[] table directly by nalType with no bounds check. nalType is
 *    a 5-bit field (0-31) taken straight from the bitstream
 *    (nal_unit_type), but the table only covers indices 0-21 -- types
 *    22-31 are legal on the wire (reserved/unspecified) and are exactly
 *    the kind of value a malformed or adversarial stream can carry, since
 *    this code explicitly expects to parse malformed streams (see the
 *    "in 0x2000 mode we catch audio using this filter" comments below).
 *    Confirmed a real global-buffer-overflow via ASan. Fixed by
 *    bounds-checking before indexing.
 *
 * 3. h264_nal_get_slice_type_for_nal(), h264_nal_get_slice_type(), and
 *    h264_slice_counter_write_packet()'s internal slice-type parsing all
 *    guarded the Exp-Golomb-decoded slice_type with only
 *    `slice_type < MAX_H264_SLICE_TYPES`, never checking `>= 0`.
 *    NALBitReader_read_ue() returns -1 on a truncated/malformed bitstream
 *    (very plausible input for this code, see above), and -1 satisfies
 *    `< MAX_H264_SLICE_TYPES`. In h264_slice_counter_write_packet() this
 *    flows into `s->slice[slice_type].count++` -- a real, reproducible
 *    heap-buffer-overflow on a single truncated packet, confirmed via
 *    ASan. Fixed by requiring `slice_type >= 0` at all three sites, and
 *    defensively in h264_slice_name_ascii() itself (also public API).
 *
 * 4. h264_slice_counter_s.nextHistoryPos was never initialized: the
 *    counter is malloc()'d (not calloc()'d) and
 *    h264_slice_counter_reset() never touched the field. The very first
 *    history write does `sliceHistory[nextHistoryPos++ %
 *    H264_SLICE_COUNTER_HISTORY_LENGTH]`, and C's '%' on a negative
 *    (indeterminate garbage) left operand can itself be negative --
 *    confirmed via UBSan/ASan as a real out-of-bounds write. Fixed by
 *    zeroing nextHistoryPos in reset().
 *
 * See test_nal_h265.c for two further bugs shared with / specific to the
 * H.265 sibling file (found via the same style of testing here).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libltntstools/nal_h264.h>

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* h264Nals_lookupType() and h265Nals_lookupType()'s sibling below are real,
 * non-static, exported symbols, but were never added to the public header.
 * Declare them locally so the regression fix for their bounds-check can be
 * exercised directly rather than only indirectly.
 */
extern const char *h264Nals_lookupType(int nalType);

/* -------- ltn_nal_h264_findHeader -------- */

static void test_findHeader_finds_start_code(void)
{
	const uint8_t buf[] = { 0xFF, 0x00, 0x00, 0x01, 0x67, 0xAA };
	int offset = -1;
	int ret = ltn_nal_h264_findHeader(buf, sizeof(buf), &offset);
	CHECK(ret == 0);
	CHECK(offset == 1);
}

static void test_findHeader_skips_forbidden_zero_bit(void)
{
	/* First "start code" at offset 0 is followed by a byte with the
	 * forbidden_zero_bit set (0x81 & 0x80), so it must be rejected and
	 * the real one at offset 5 returned instead.
	 */
	const uint8_t buf[] = { 0x00,0x00,0x01, 0x81, 0xFF, 0x00,0x00,0x01, 0x07, 0xAA };
	int offset = -1;
	int ret = ltn_nal_h264_findHeader(buf, sizeof(buf), &offset);
	CHECK(ret == 0);
	CHECK(offset == 5);
}

static void test_findHeader_returns_error_when_not_found(void)
{
	const uint8_t buf[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
	int offset = -1;
	int ret = ltn_nal_h264_findHeader(buf, sizeof(buf), &offset);
	CHECK(ret != 0);
}

/* -------- ltn_nal_h264_find_headers -------- */

static void test_find_headers_multiple_nals(void)
{
	const uint8_t buf[] = {
		0,0,1, 0x67, 0xAA, 0xBB,             /* SPS  (type 7), 6 bytes */
		0,0,1, 0x68, 0xCC,                   /* PPS  (type 8), 5 bytes */
		0,0,1, 0x65, 0xDD, 0xEE, 0xFF,       /* IDR  (type 5), 7 bytes (to end of buffer) */
	};

	struct ltn_nal_headers_s *arr = NULL;
	int arrLen = 0;
	int ret = ltn_nal_h264_find_headers(buf, sizeof(buf), &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == 3);
	if (arrLen == 3) {
		CHECK(arr[0].nalType == 7);
		CHECK(arr[0].lengthBytes == 6);
		CHECK(strcmp(arr[0].nalName, "SPS") == 0);

		CHECK(arr[1].nalType == 8);
		CHECK(arr[1].lengthBytes == 5);
		CHECK(strcmp(arr[1].nalName, "PPS") == 0);

		CHECK(arr[2].nalType == 5);
		CHECK(arr[2].lengthBytes == 7);
		CHECK(arr[2].ptr == buf + 11);
	}
	free(arr);
}

static void test_find_headers_no_start_codes(void)
{
	const uint8_t buf[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	struct ltn_nal_headers_s *arr = NULL;
	int arrLen = -1;
	int ret = ltn_nal_h264_find_headers(buf, sizeof(buf), &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == 0);
	free(arr);
}

static void test_find_headers_grows_past_initial_capacity(void)
{
	/* Regression: the array starts at 64 entries. This has always grown
	 * correctly in nal_h264.c (unlike its nal_h265.c sibling, which
	 * didn't -- see test_nal_h265.c), but is worth locking down.
	 */
	const int n = 70, per = 6;
	uint8_t *buf = malloc(n * per);
	CHECK(buf != NULL);
	if (!buf)
		return;

	for (int i = 0; i < n; i++) {
		uint8_t *p = buf + (i * per);
		p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 0x67; p[4] = 0xAA; p[5] = 0xBB;
	}

	struct ltn_nal_headers_s *arr = NULL;
	int arrLen = 0;
	int ret = ltn_nal_h264_find_headers(buf, n * per, &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == n);
	if (arrLen == n) {
		CHECK(arr[n - 1].lengthBytes == (uint32_t)per);
	}
	free(arr);
	free(buf);
}

/* -------- ltn_nal_h264_strip_emulation_prevention -------- */

static void test_strip_emulation_prevention_removes_one_marker(void)
{
	uint8_t buf[] = { 0x00,0x00,0x01, 0x67, 0xAA, 0x00,0x00,0x03,0x01, 0xBB };
	struct ltn_nal_headers_s h = { .ptr = buf, .lengthBytes = sizeof(buf) };

	int ret = ltn_nal_h264_strip_emulation_prevention(&h);
	CHECK(ret == 0);
	CHECK(h.lengthBytes == 9);

	const uint8_t expect[] = { 0x00,0x00,0x01, 0x67, 0xAA, 0x00,0x00,0x01, 0xBB };
	CHECK(memcmp(h.ptr, expect, sizeof(expect)) == 0);
}

static void test_strip_emulation_prevention_removes_multiple_markers(void)
{
	uint8_t buf[] = { 0x00,0x00,0x01, 0x67, 0x00,0x00,0x03,0x00, 0x00,0x00,0x03,0x01, 0xBB };
	struct ltn_nal_headers_s h = { .ptr = buf, .lengthBytes = sizeof(buf) };

	int ret = ltn_nal_h264_strip_emulation_prevention(&h);
	CHECK(ret == 0);
	CHECK(h.lengthBytes == 11);

	const uint8_t expect[] = { 0x00,0x00,0x01, 0x67, 0x00,0x00,0x00,0x00,0x00,0x01, 0xBB };
	CHECK(memcmp(h.ptr, expect, sizeof(expect)) == 0);
}

static void test_strip_emulation_prevention_rejects_bad_input(void)
{
	CHECK(ltn_nal_h264_strip_emulation_prevention(NULL) < 0);

	struct ltn_nal_headers_s null_ptr = { .ptr = NULL, .lengthBytes = 10 };
	CHECK(ltn_nal_h264_strip_emulation_prevention(&null_ptr) < 0);

	uint8_t tiny_buf[3] = { 0, 0, 1 };
	struct ltn_nal_headers_s too_short = { .ptr = tiny_buf, .lengthBytes = sizeof(tiny_buf) };
	CHECK(ltn_nal_h264_strip_emulation_prevention(&too_short) < 0);
}

/* -------- ltn_nal_h264_findNalTypes -------- */

static void test_findNalTypes_lists_names_in_order(void)
{
	const uint8_t buf[] = {
		0,0,1, 0x67, 0xAA, 0xBB,
		0,0,1, 0x68, 0xCC,
		0,0,1, 0x65, 0xDD, 0xEE, 0xFF,
	};
	char *types = ltn_nal_h264_findNalTypes(buf, sizeof(buf));
	CHECK(types != NULL);
	if (types) {
		CHECK(strcmp(types, "SPS, PPS, slice_layer_without_partitioning_rbsp IDR") == 0);
	}
	free(types);
}

static void test_findNalTypes_no_nals_returns_null(void)
{
	const uint8_t buf[] = { 0xAA, 0xBB, 0xCC };
	char *types = ltn_nal_h264_findNalTypes(buf, sizeof(buf));
	CHECK(types == NULL);
	free(types);
}

/* -------- h264Nals_lookupName / h264Nals_lookupType -------- */

static void test_lookupName_known_values(void)
{
	CHECK(strcmp(h264Nals_lookupName(0), "UNSPECIFIED") == 0);
	CHECK(strcmp(h264Nals_lookupName(7), "SPS") == 0);
	CHECK(strcmp(h264Nals_lookupName(8), "PPS") == 0);
	CHECK(strcmp(h264Nals_lookupType(5), "IDR") == 0);
}

static void test_lookupName_out_of_range_is_bounds_checked(void)
{
	/* Regression: nalType is masked with 0x1f by every caller (5 bits,
	 * 0-31), but the table only covers 0-21. 22-31 must not read past
	 * the table.
	 */
	CHECK(strcmp(h264Nals_lookupName(25), "RESERVED/UNKNOWN") == 0);
	CHECK(strcmp(h264Nals_lookupName(31), "RESERVED/UNKNOWN") == 0);
	CHECK(strcmp(h264Nals_lookupType(25), "") == 0);
	CHECK(strcmp(h264Nals_lookupName(-1), "RESERVED/UNKNOWN") == 0);
}

/* -------- slice_type classification -------- */

static void test_is_slice_type_iframe(void)
{
	CHECK(h264_is_slice_type_iframe(2));
	CHECK(h264_is_slice_type_iframe(4));
	CHECK(h264_is_slice_type_iframe(7));
	CHECK(h264_is_slice_type_iframe(9));
	CHECK(!h264_is_slice_type_iframe(0));
	CHECK(!h264_is_slice_type_iframe(1));
	CHECK(!h264_is_slice_type_iframe(3));
}

static void test_is_slice_type_pframe(void)
{
	CHECK(h264_is_slice_type_pframe(0));
	CHECK(h264_is_slice_type_pframe(3));
	CHECK(h264_is_slice_type_pframe(5));
	CHECK(h264_is_slice_type_pframe(8));
	CHECK(!h264_is_slice_type_pframe(1));
	CHECK(!h264_is_slice_type_pframe(2));
}

static void test_is_slice_type_bframe(void)
{
	CHECK(h264_is_slice_type_bframe(1));
	CHECK(h264_is_slice_type_bframe(6));
	CHECK(!h264_is_slice_type_bframe(0));
	CHECK(!h264_is_slice_type_bframe(2));
}

static void test_slice_name_ascii_known_values(void)
{
	CHECK(strcmp(h264_slice_name_ascii(0), "P") == 0);
	CHECK(strcmp(h264_slice_name_ascii(1), "B") == 0);
	CHECK(strcmp(h264_slice_name_ascii(2), "I") == 0);
	CHECK(strcmp(h264_slice_name_ascii(3), "p") == 0);
	CHECK(strcmp(h264_slice_name_ascii(4), "i") == 0);
}

static void test_slice_name_ascii_wraps_at_max(void)
{
	/* MAX_H264_SLICE_TYPES == 10, so 10 wraps back to slice_defaults[0]. */
	CHECK(strcmp(h264_slice_name_ascii(10), h264_slice_name_ascii(0)) == 0);
}

static void test_slice_name_ascii_negative_input_is_safe(void)
{
	/* Regression: slice_type % MAX_H264_SLICE_TYPES on a negative
	 * slice_type is itself negative in C -- must not index the array.
	 */
	CHECK(strcmp(h264_slice_name_ascii(-1), "?") == 0);
}

/* -------- h264_nal_get_slice_type_for_nal -------- */

static void test_get_slice_type_for_nal_returns_iframe_type(void)
{
	/* first_mb_in_slice ue(0) = "1"; slice_type ue(2) = "011" -> byte 0xB0 */
	uint8_t buf[] = { 0x00,0x00,0x01, 0x41, 0xB0, 0x00,0x00,0x00 };
	struct ltn_nal_headers_s hdr = { .ptr = buf, .lengthBytes = sizeof(buf), .nalType = 1 };

	unsigned int sliceType = 0xFFFFFFFF;
	int ret = h264_nal_get_slice_type_for_nal(&hdr, &sliceType);
	CHECK(ret == 0);
	CHECK(sliceType == 2);
	CHECK(h264_is_slice_type_iframe(sliceType));
}

static void test_get_slice_type_for_nal_rejects_non_slice_nal(void)
{
	uint8_t buf[] = { 0x00,0x00,0x01, 0x67, 0xAA, 0xBB, 0xCC, 0xDD };
	struct ltn_nal_headers_s hdr = { .ptr = buf, .lengthBytes = sizeof(buf), .nalType = 7 /* SPS */ };

	unsigned int sliceType = 0;
	int ret = h264_nal_get_slice_type_for_nal(&hdr, &sliceType);
	CHECK(ret != 0);
}

static void test_get_slice_type_for_nal_rejects_null_args(void)
{
	struct ltn_nal_headers_s hdr = { 0 };
	unsigned int sliceType = 0;
	CHECK(h264_nal_get_slice_type_for_nal(NULL, &sliceType) < 0);
	CHECK(h264_nal_get_slice_type_for_nal(&hdr, NULL) < 0);
}

static void test_get_slice_type_for_nal_malformed_does_not_underflow(void)
{
	/* Regression: first_mb_in_slice ue(0) = "1", then all-zero bits ->
	 * slice_type's Exp-Golomb read runs out of bits and returns -1.
	 * Must not be reported as a valid slice type.
	 */
	uint8_t buf[] = { 0x00,0x00,0x01, 0x41, 0x80, 0x00,0x00,0x00 };
	struct ltn_nal_headers_s hdr = { .ptr = buf, .lengthBytes = sizeof(buf), .nalType = 1 };

	unsigned int sliceType = 0xFFFFFFFF;
	int ret = h264_nal_get_slice_type_for_nal(&hdr, &sliceType);
	CHECK(ret != 0);
	CHECK(sliceType == 0xFFFFFFFF); /* untouched */
}

/* -------- h264_nal_get_slice_type -------- */

static void test_get_slice_type_finds_and_names_iframe(void)
{
	uint8_t buf[] = { 0x00,0x00,0x01, 0x41, 0xB0, 0x00,0x00,0x00 };
	struct ltn_nal_headers_s hdr = { .ptr = buf, .lengthBytes = sizeof(buf) };

	char name[8] = { 0 };
	int ret = h264_nal_get_slice_type(&hdr, name);
	CHECK(ret == 0);
	CHECK(strcmp(name, "I") == 0);
}

/* -------- ltn_sei_h264_lookupName -------- */

static void test_sei_lookupName_known_and_unknown(void)
{
	CHECK(strcmp(ltn_sei_h264_lookupName(0), "buffer_period") == 0);
	CHECK(strcmp(ltn_sei_h264_lookupName(6), "recovery_point") == 0);
	/* MAX_H264_SEI == 18; 19 and above fall back to the "undefined" entry. */
	CHECK(strcmp(ltn_sei_h264_lookupName(19), "undefined in spec 2004") == 0);
	CHECK(strcmp(ltn_sei_h264_lookupName(1000), "undefined in spec 2004") == 0);
}

/* -------- ltn_sei_h264_find_headers -------- */

static void test_sei_find_headers_extracts_only_sei_nals(void)
{
	uint8_t sei_buf[] = { 0,0,1, 0x06, 0x01 /* seiType = pic_timing */, 0xAA, 0xBB };
	uint8_t pps_buf[] = { 0,0,1, 0x08, 0xCC };

	struct ltn_nal_headers_s nals[2] = {
		{ .ptr = sei_buf, .lengthBytes = sizeof(sei_buf), .nalType = 6, .nalName = "SEI" },
		{ .ptr = pps_buf, .lengthBytes = sizeof(pps_buf), .nalType = 8, .nalName = "PPS" },
	};

	struct ltn_sei_headers_s *arr = NULL;
	int arrLen = -1;
	int ret = ltn_sei_h264_find_headers(nals, 2, &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == 1);
	if (arrLen == 1) {
		CHECK(arr[0].seiType == 1);
		CHECK(strcmp(arr[0].seiName, "pic_timing") == 0);
		CHECK(arr[0].ptr == sei_buf);
		CHECK(arr[0].lengthBytes == sizeof(sei_buf));
	}
	free(arr);
}

static void test_sei_find_headers_rejects_null_args(void)
{
	struct ltn_sei_headers_s *arr = NULL;
	int arrLen = -1;
	CHECK(ltn_sei_h264_find_headers(NULL, 0, &arr, &arrLen) < 0);

	struct ltn_nal_headers_s nals[1] = { { .nalType = 6 } };
	CHECK(ltn_sei_h264_find_headers(nals, 1, &arr, NULL) < 0);
}

/* -------- h264_slice_counter_* (via h264_slice_counter_write) -------- */

static void build_ts_packet(uint8_t *pkt, uint16_t pid, uint8_t nal_hdr, uint8_t first_data_byte)
{
	memset(pkt, 0x00, 188);
	pkt[0] = 0x47;
	pkt[1] = (pid >> 8) & 0x1f;
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10;
	pkt[4] = 0x00;
	pkt[5] = 0x00;
	pkt[6] = 0x01;
	pkt[7] = nal_hdr;         /* nal_unit_type 1 (non-IDR slice) in low 5 bits */
	pkt[8] = first_data_byte; /* first RBSP byte read by the bit reader */
}

static void test_slice_counter_alloc_starts_zeroed(void)
{
	void *ctx = h264_slice_counter_alloc(0x100);
	CHECK(ctx != NULL);
	CHECK(h264_slice_counter_get_pid(ctx) == 0x100);

	struct h264_slice_counter_results_s r;
	h264_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0 && r.si == 0 && r.sp == 0);
	for (int i = 0; i < H264_SLICE_COUNTER_HISTORY_LENGTH; i++) {
		CHECK(r.sliceHistory[i] == ' ');
	}
	CHECK(r.sliceHistory[H264_SLICE_COUNTER_HISTORY_LENGTH] == 0);

	h264_slice_counter_free(ctx);
}

static void test_slice_counter_write_updates_counts_and_history(void)
{
	uint8_t pkts[5 * 188];
	build_ts_packet(pkts + 0*188, 0x100, 0x21, 0xB0); /* I  (slice_type 2) */
	build_ts_packet(pkts + 1*188, 0x100, 0x21, 0xC0); /* P  (slice_type 0) */
	build_ts_packet(pkts + 2*188, 0x100, 0x21, 0xA0); /* B  (slice_type 1) */
	build_ts_packet(pkts + 3*188, 0x100, 0x21, 0x90); /* SP (slice_type 3) */
	build_ts_packet(pkts + 4*188, 0x100, 0x21, 0x94); /* SI (slice_type 4) */

	void *ctx = h264_slice_counter_alloc(0x2000); /* 0x2000 == match every pid */
	h264_slice_counter_write(ctx, pkts, 5);

	struct h264_slice_counter_results_s r;
	h264_slice_counter_query(ctx, &r);
	CHECK(r.i == 1);
	CHECK(r.p == 1);
	CHECK(r.b == 1);
	CHECK(r.sp == 1);
	CHECK(r.si == 1);
	CHECK(strcmp(r.sliceHistory + (H264_SLICE_COUNTER_HISTORY_LENGTH - 5), "IPBpi") == 0);

	h264_slice_counter_free(ctx);
}

static void test_slice_counter_write_ignores_non_matching_pid(void)
{
	uint8_t pkt[188];
	build_ts_packet(pkt, 0x100, 0x21, 0xB0); /* I slice on pid 0x100 */

	void *ctx = h264_slice_counter_alloc(0x200); /* tracking a different pid */
	h264_slice_counter_write(ctx, pkt, 1);

	struct h264_slice_counter_results_s r;
	h264_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0);

	h264_slice_counter_free(ctx);
}

static void test_slice_counter_reset_clears_counts(void)
{
	uint8_t pkt[188];
	build_ts_packet(pkt, 0x100, 0x21, 0xB0);

	void *ctx = h264_slice_counter_alloc(0x2000);
	h264_slice_counter_write(ctx, pkt, 1);

	struct h264_slice_counter_results_s r;
	h264_slice_counter_query(ctx, &r);
	CHECK(r.i == 1);

	h264_slice_counter_reset(ctx);
	h264_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0);
	CHECK(r.sliceHistory[H264_SLICE_COUNTER_HISTORY_LENGTH - 1] == ' ');

	h264_slice_counter_free(ctx);
}

static void test_slice_counter_reset_pid_changes_tracked_pid(void)
{
	void *ctx = h264_slice_counter_alloc(0x100);
	CHECK(h264_slice_counter_get_pid(ctx) == 0x100);

	h264_slice_counter_reset_pid(ctx, 0x200);
	CHECK(h264_slice_counter_get_pid(ctx) == 0x200);

	h264_slice_counter_free(ctx);
}

static void test_slice_counter_history_wraps_without_corruption(void)
{
	/* Regression coverage for the nextHistoryPos-uninitialized bug: feed
	 * more updates than H264_SLICE_COUNTER_HISTORY_LENGTH and confirm the
	 * counter survives and the history string is still exactly LENGTH
	 * chars, all 'I' (the ring should just keep overwriting itself).
	 */
	uint8_t pkts[25 * 188];
	for (int i = 0; i < 25; i++) {
		build_ts_packet(pkts + i*188, 0x100, 0x21, 0xB0); /* I every time */
	}

	void *ctx = h264_slice_counter_alloc(0x2000);
	h264_slice_counter_write(ctx, pkts, 25);

	struct h264_slice_counter_results_s r;
	h264_slice_counter_query(ctx, &r);
	CHECK(r.i == 25);
	CHECK((int)strlen(r.sliceHistory) == H264_SLICE_COUNTER_HISTORY_LENGTH);
	for (int i = 0; i < H264_SLICE_COUNTER_HISTORY_LENGTH; i++) {
		CHECK(r.sliceHistory[i] == 'I');
	}

	h264_slice_counter_free(ctx);
}

static void test_slice_counter_malformed_slice_does_not_crash(void)
{
	/* Regression: first_mb_in_slice ue(0) = "1", remaining bits all zero
	 * -> slice_type's Exp-Golomb read fails (-1). Must be treated as
	 * malformed, not indexed into the counts array.
	 */
	uint8_t pkt[188];
	build_ts_packet(pkt, 0x100, 0x21, 0x80);

	void *ctx = h264_slice_counter_alloc(0x2000);
	h264_slice_counter_write(ctx, pkt, 1); /* must not crash / corrupt memory */

	struct h264_slice_counter_results_s r;
	h264_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0 && r.si == 0 && r.sp == 0);

	h264_slice_counter_free(ctx);
}

/* -------- ltn_nal_h264_parse_pic_timing -------- */

/* Minimal MSB-first bit packer, used to build synthetic PIC_TIMING SEI
 * payloads with known field values so the decoder's output can be checked
 * exactly, without hand-deriving expected hex bytes.
 */
struct testbits_s
{
	uint8_t buf[64];
	int bitpos;
};

static void testbits_push(struct testbits_s *bw, uint32_t value, int nbits)
{
	for (int i = nbits - 1; i >= 0; i--) {
		int bit = (value >> i) & 1;
		int byteIdx = bw->bitpos / 8;
		int bitIdx = 7 - (bw->bitpos % 8);
		if (bit) {
			bw->buf[byteIdx] |= (1 << bitIdx);
		} else {
			bw->buf[byteIdx] &= ~(1 << bitIdx);
		}
		bw->bitpos++;
	}
}

/* Builds a full NAL buffer (5-byte start code/type/payloadType header,
 * matching what ltn_nal_h264_parse_pic_timing() expects at buf[0..4],
 * followed by the bit-packed SEI payload) and returns its length.
 */
static int build_pic_timing_nal(uint8_t *out, struct testbits_s *bw)
{
	out[0] = 0x00;
	out[1] = 0x00;
	out[2] = 0x01;
	out[3] = 0x06; /* nal_unit_type = SEI */
	out[4] = 0x01; /* SEI payloadType = pic_timing */
	int paylen = (bw->bitpos + 7) / 8;
	memcpy(&out[5], bw->buf, paylen);
	return 5 + paylen;
}

static void test_pic_timing_null_args_rejected(void)
{
	uint8_t buf[16] = { 0 };
	struct ltn_nal_h264_pic_timing_s result;

	CHECK(ltn_nal_h264_parse_pic_timing(NULL, sizeof(buf), 0, 0, -1, &result) < 0);
	CHECK(ltn_nal_h264_parse_pic_timing(buf, sizeof(buf), 0, 0, -1, NULL) < 0);
}

static void test_pic_timing_too_short_rejected(void)
{
	uint8_t buf[4] = { 0x00, 0x00, 0x01, 0x06 }; /* lengthBytes < 5 */
	struct ltn_nal_h264_pic_timing_s result;

	CHECK(ltn_nal_h264_parse_pic_timing(buf, sizeof(buf), 0, 0, -1, &result) < 0);
}

static void test_pic_timing_exhausted_bitstream_rejected(void)
{
	/* Exactly 5 bytes -- header only, zero payload bits available, so the
	 * pic_struct read runs off the end of the buffer and must be treated
	 * as an error rather than silently returning garbage.
	 */
	uint8_t buf[5] = { 0x00, 0x00, 0x01, 0x06, 0x01 };
	struct ltn_nal_h264_pic_timing_s result;

	CHECK(ltn_nal_h264_parse_pic_timing(buf, sizeof(buf), 0, 0, -1, &result) < 0);
}

static void test_pic_timing_invalid_override_rejected(void)
{
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 0, 4); /* pic_struct = 0 (unused, overridden below) */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 0, 0, 99, &result) < 0);
}

static void test_pic_timing_full_timestamp_flag_path(void)
{
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 0, 4);   /* pic_struct = 0 -> NumClockTS = 1 */
	testbits_push(&bw, 1, 1);   /* clock_timestamp_flag = 1 */
	testbits_push(&bw, 1, 2);   /* ct_type */
	testbits_push(&bw, 1, 1);   /* nuit_field_based_flag */
	testbits_push(&bw, 5, 5);   /* counting_type */
	testbits_push(&bw, 1, 1);   /* full_timestamp_flag = 1 */
	testbits_push(&bw, 1, 1);   /* discontinuity_flag */
	testbits_push(&bw, 0, 1);   /* cnt_dropped_flag */
	testbits_push(&bw, 25, 8);  /* n_frames */
	testbits_push(&bw, 45, 6);  /* seconds */
	testbits_push(&bw, 30, 6);  /* minutes */
	testbits_push(&bw, 12, 5);  /* hours */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 0, 0, -1, &result) == 0);
	CHECK(result.pic_struct == 0);
	CHECK(result.clockCount == 1);
	CHECK(result.clocks[0].present == 1);
	CHECK(result.clocks[0].ct_type == 1);
	CHECK(result.clocks[0].nuit_field_based_flag == 1);
	CHECK(result.clocks[0].counting_type == 5);
	CHECK(result.clocks[0].full_timestamp_flag == 1);
	CHECK(result.clocks[0].discontinuity_flag == 1);
	CHECK(result.clocks[0].cnt_dropped_flag == 0);
	CHECK(result.clocks[0].n_frames == 25);
	CHECK(result.clocks[0].seconds == 45);
	CHECK(result.clocks[0].minutes == 30);
	CHECK(result.clocks[0].hours == 12);
}

static void test_pic_timing_nested_seconds_minutes_hours_path(void)
{
	/* full_timestamp_flag = 0 but seconds_flag/minutes_flag/hours_flag
	 * all set -- must decode identically to the full_timestamp_flag = 1
	 * path via the nested branch.
	 */
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 0, 4);   /* pic_struct = 0 -> NumClockTS = 1 */
	testbits_push(&bw, 1, 1);   /* clock_timestamp_flag = 1 */
	testbits_push(&bw, 2, 2);   /* ct_type */
	testbits_push(&bw, 0, 1);   /* nuit_field_based_flag */
	testbits_push(&bw, 10, 5);  /* counting_type */
	testbits_push(&bw, 0, 1);   /* full_timestamp_flag = 0 */
	testbits_push(&bw, 0, 1);   /* discontinuity_flag */
	testbits_push(&bw, 1, 1);   /* cnt_dropped_flag */
	testbits_push(&bw, 200, 8); /* n_frames */
	testbits_push(&bw, 1, 1);   /* seconds_flag = 1 */
	testbits_push(&bw, 33, 6);  /* seconds */
	testbits_push(&bw, 1, 1);   /* minutes_flag = 1 */
	testbits_push(&bw, 22, 6);  /* minutes */
	testbits_push(&bw, 1, 1);   /* hours_flag = 1 */
	testbits_push(&bw, 7, 5);   /* hours */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 0, 0, -1, &result) == 0);
	CHECK(result.clocks[0].full_timestamp_flag == 0);
	CHECK(result.clocks[0].discontinuity_flag == 0);
	CHECK(result.clocks[0].cnt_dropped_flag == 1);
	CHECK(result.clocks[0].n_frames == 200);
	CHECK(result.clocks[0].seconds == 33);
	CHECK(result.clocks[0].minutes == 22);
	CHECK(result.clocks[0].hours == 7);
}

static void test_pic_timing_seconds_only_path(void)
{
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 0, 4);  /* pic_struct = 0 -> NumClockTS = 1 */
	testbits_push(&bw, 1, 1);  /* clock_timestamp_flag = 1 */
	testbits_push(&bw, 0, 2);  /* ct_type */
	testbits_push(&bw, 0, 1);  /* nuit_field_based_flag */
	testbits_push(&bw, 0, 5);  /* counting_type */
	testbits_push(&bw, 0, 1);  /* full_timestamp_flag = 0 */
	testbits_push(&bw, 0, 1);  /* discontinuity_flag */
	testbits_push(&bw, 0, 1);  /* cnt_dropped_flag */
	testbits_push(&bw, 0, 8);  /* n_frames */
	testbits_push(&bw, 1, 1);  /* seconds_flag = 1 */
	testbits_push(&bw, 50, 6); /* seconds */
	testbits_push(&bw, 0, 1);  /* minutes_flag = 0 */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 0, 0, -1, &result) == 0);
	CHECK(result.clocks[0].seconds == 50);
	CHECK(result.clocks[0].minutes == -1);
	CHECK(result.clocks[0].hours == -1);
}

static void test_pic_timing_no_seconds_path(void)
{
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 0, 4); /* pic_struct = 0 -> NumClockTS = 1 */
	testbits_push(&bw, 1, 1); /* clock_timestamp_flag = 1 */
	testbits_push(&bw, 0, 2); /* ct_type */
	testbits_push(&bw, 0, 1); /* nuit_field_based_flag */
	testbits_push(&bw, 0, 5); /* counting_type */
	testbits_push(&bw, 0, 1); /* full_timestamp_flag = 0 */
	testbits_push(&bw, 0, 1); /* discontinuity_flag */
	testbits_push(&bw, 0, 1); /* cnt_dropped_flag */
	testbits_push(&bw, 0, 8); /* n_frames */
	testbits_push(&bw, 0, 1); /* seconds_flag = 0 */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 0, 0, -1, &result) == 0);
	CHECK(result.clocks[0].seconds == -1);
	CHECK(result.clocks[0].minutes == -1);
	CHECK(result.clocks[0].hours == -1);
}

static void test_pic_timing_multi_clock_advances_correctly(void)
{
	/* pic_struct = 3 -> NumClockTS = 2. First clock absent, second
	 * present -- verifies the loop advances past an absent clock entry
	 * without consuming bits meant for the next one.
	 */
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 3, 4);   /* pic_struct = 3 -> NumClockTS = 2 */
	testbits_push(&bw, 0, 1);   /* clocks[0].clock_timestamp_flag = 0 */
	testbits_push(&bw, 1, 1);   /* clocks[1].clock_timestamp_flag = 1 */
	testbits_push(&bw, 3, 2);   /* ct_type */
	testbits_push(&bw, 1, 1);   /* nuit_field_based_flag */
	testbits_push(&bw, 31, 5);  /* counting_type */
	testbits_push(&bw, 1, 1);   /* full_timestamp_flag = 1 */
	testbits_push(&bw, 0, 1);   /* discontinuity_flag */
	testbits_push(&bw, 1, 1);   /* cnt_dropped_flag */
	testbits_push(&bw, 59, 8);  /* n_frames */
	testbits_push(&bw, 59, 6);  /* seconds */
	testbits_push(&bw, 59, 6);  /* minutes */
	testbits_push(&bw, 23, 5);  /* hours */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 0, 0, -1, &result) == 0);
	CHECK(result.pic_struct == 3);
	CHECK(result.clockCount == 2);
	CHECK(result.clocks[0].present == 0);
	CHECK(result.clocks[1].present == 1);
	CHECK(result.clocks[1].ct_type == 3);
	CHECK(result.clocks[1].nuit_field_based_flag == 1);
	CHECK(result.clocks[1].counting_type == 31);
	CHECK(result.clocks[1].n_frames == 59);
	CHECK(result.clocks[1].seconds == 59);
	CHECK(result.clocks[1].minutes == 59);
	CHECK(result.clocks[1].hours == 23);
}

static void test_pic_timing_cpb_dpb_lengths_are_skipped_correctly(void)
{
	/* Non-zero cpb/dpb_removal_delay_length values, with garbage bits in
	 * those fields, must be fully consumed (and ignored) so the following
	 * pic_struct/clock fields still decode at the correct bit offset.
	 */
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 0xAB, 8); /* cpb_removal_delay (garbage) */
	testbits_push(&bw, 0x5, 4);  /* dpb_removal_delay (garbage) */
	testbits_push(&bw, 0, 4);    /* pic_struct = 0 -> NumClockTS = 1 */
	testbits_push(&bw, 1, 1);    /* clock_timestamp_flag = 1 */
	testbits_push(&bw, 0, 2);    /* ct_type */
	testbits_push(&bw, 0, 1);    /* nuit_field_based_flag */
	testbits_push(&bw, 0, 5);    /* counting_type */
	testbits_push(&bw, 1, 1);    /* full_timestamp_flag = 1 */
	testbits_push(&bw, 0, 1);    /* discontinuity_flag */
	testbits_push(&bw, 0, 1);    /* cnt_dropped_flag */
	testbits_push(&bw, 1, 8);    /* n_frames */
	testbits_push(&bw, 2, 6);    /* seconds */
	testbits_push(&bw, 3, 6);    /* minutes */
	testbits_push(&bw, 4, 5);    /* hours */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 8, 4, -1, &result) == 0);
	CHECK(result.clocks[0].n_frames == 1);
	CHECK(result.clocks[0].seconds == 2);
	CHECK(result.clocks[0].minutes == 3);
	CHECK(result.clocks[0].hours == 4);
}

static void test_pic_timing_pic_struct_override_forces_value(void)
{
	/* pic_struct read from the bitstream is 0 (-> NumClockTS 1), but the
	 * caller forces pic_struct = 8 (-> NumClockTS 3), matching how
	 * pes_inspector.c overrides pic_struct for encoders known to
	 * populate it incorrectly.
	 */
	struct testbits_s bw = { 0 };
	testbits_push(&bw, 0, 4); /* pic_struct = 0 in the bitstream */
	testbits_push(&bw, 0, 1); /* clocks[0].clock_timestamp_flag = 0 */
	testbits_push(&bw, 0, 1); /* clocks[1].clock_timestamp_flag = 0 */
	testbits_push(&bw, 0, 1); /* clocks[2].clock_timestamp_flag = 0 */

	uint8_t nal[64];
	int len = build_pic_timing_nal(nal, &bw);

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, len, 0, 0, 8, &result) == 0);
	CHECK(result.pic_struct == 8);
	CHECK(result.clockCount == 3);
	CHECK(result.clocks[0].present == 0);
	CHECK(result.clocks[1].present == 0);
	CHECK(result.clocks[2].present == 0);
}

static void test_pic_timing_real_capture_matches_ltn_encoder(void)
{
	/* A real PIC_TIMING SEI captured from an LTN encoder (see the comment
	 * dump in ltntstools/src/pes_inspector.c's _parse_PIC_TIMING()),
	 * decoded by hand against this exact bit layout to confirm it
	 * produces "15:18:52.31 disc:0 ct:0 counting_type:0 nuit:1
	 * full_timestamp:1 cnt_dropped:0" with the default cpb/dpb lengths
	 * (15/11) and pic_struct forced to 8, matching the "Video Engine"
	 * pid override in pes_inspector.c.
	 */
	uint8_t nal[16] = {
		0x00, 0x00, 0x01, 0x06, 0x01, 0x08, 0x02, 0x60,
		0x80, 0x90, 0x41, 0xfd, 0x12, 0x7c, 0x80, 0x00,
	};

	struct ltn_nal_h264_pic_timing_s result;
	CHECK(ltn_nal_h264_parse_pic_timing(nal, sizeof(nal), 15, 11, 8, &result) == 0);
	CHECK(result.pic_struct == 8);
	CHECK(result.clockCount == 3);
	CHECK(result.clocks[0].present == 0);
	CHECK(result.clocks[1].present == 0);
	CHECK(result.clocks[2].present == 1);
	CHECK(result.clocks[2].ct_type == 0);
	CHECK(result.clocks[2].nuit_field_based_flag == 1);
	CHECK(result.clocks[2].counting_type == 0);
	CHECK(result.clocks[2].full_timestamp_flag == 1);
	CHECK(result.clocks[2].discontinuity_flag == 0);
	CHECK(result.clocks[2].cnt_dropped_flag == 0);
	CHECK(result.clocks[2].n_frames == 31);
	CHECK(result.clocks[2].hours == 15);
	CHECK(result.clocks[2].minutes == 18);
	CHECK(result.clocks[2].seconds == 52);

	/* Same capture, but without the pid override -- pic_struct is read
	 * as 0 straight from the bitstream (-> NumClockTS 1), and that lone
	 * clock entry is absent.
	 */
	CHECK(ltn_nal_h264_parse_pic_timing(nal, sizeof(nal), 15, 11, -1, &result) == 0);
	CHECK(result.pic_struct == 0);
	CHECK(result.clockCount == 1);
	CHECK(result.clocks[0].present == 0);
}

int main(void)
{
	test_findHeader_finds_start_code();
	test_findHeader_skips_forbidden_zero_bit();
	test_findHeader_returns_error_when_not_found();

	test_find_headers_multiple_nals();
	test_find_headers_no_start_codes();
	test_find_headers_grows_past_initial_capacity();

	test_strip_emulation_prevention_removes_one_marker();
	test_strip_emulation_prevention_removes_multiple_markers();
	test_strip_emulation_prevention_rejects_bad_input();

	test_findNalTypes_lists_names_in_order();
	test_findNalTypes_no_nals_returns_null();

	test_lookupName_known_values();
	test_lookupName_out_of_range_is_bounds_checked();

	test_is_slice_type_iframe();
	test_is_slice_type_pframe();
	test_is_slice_type_bframe();

	test_slice_name_ascii_known_values();
	test_slice_name_ascii_wraps_at_max();
	test_slice_name_ascii_negative_input_is_safe();

	test_get_slice_type_for_nal_returns_iframe_type();
	test_get_slice_type_for_nal_rejects_non_slice_nal();
	test_get_slice_type_for_nal_rejects_null_args();
	test_get_slice_type_for_nal_malformed_does_not_underflow();

	test_get_slice_type_finds_and_names_iframe();

	test_sei_lookupName_known_and_unknown();
	test_sei_find_headers_extracts_only_sei_nals();
	test_sei_find_headers_rejects_null_args();

	test_slice_counter_alloc_starts_zeroed();
	test_slice_counter_write_updates_counts_and_history();
	test_slice_counter_write_ignores_non_matching_pid();
	test_slice_counter_reset_clears_counts();
	test_slice_counter_reset_pid_changes_tracked_pid();
	test_slice_counter_history_wraps_without_corruption();
	test_slice_counter_malformed_slice_does_not_crash();

	test_pic_timing_null_args_rejected();
	test_pic_timing_too_short_rejected();
	test_pic_timing_exhausted_bitstream_rejected();
	test_pic_timing_invalid_override_rejected();
	test_pic_timing_full_timestamp_flag_path();
	test_pic_timing_nested_seconds_minutes_hours_path();
	test_pic_timing_seconds_only_path();
	test_pic_timing_no_seconds_path();
	test_pic_timing_multi_clock_advances_correctly();
	test_pic_timing_cpb_dpb_lengths_are_skipped_correctly();
	test_pic_timing_pic_struct_override_forces_value();
	test_pic_timing_real_capture_matches_ltn_encoder();

	if (g_failures == 0) {
		printf("PASS: all nal_h264 tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d nal_h264 test(s) failed\n", g_failures);
	return 1;
}
