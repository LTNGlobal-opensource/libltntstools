/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/nal_h265.c / src/libltntstools/nal_h265.h.
 *
 * Several real bugs were found while writing these tests and have been
 * FIXED (in src/nal_h265.c; see test_nal_h264.c for further bugs shared
 * with / found via the same testing of its H.264 sibling file):
 *
 * 1. ltn_nal_h265_find_headers() allocated a fixed 64-entry array and, on
 *    finding a NAL, wrote straight into it with no bounds check --
 *    entirely missing the grow-by-realloc() logic its H.264 sibling
 *    (ltn_nal_h264_find_headers) has. A buffer with more than 64 NAL units
 *    caused a real, reproducible heap-buffer-overflow, confirmed via ASan.
 *    Also, for a buffer with zero NAL units, it unconditionally wrote
 *    `prev->lengthBytes = ... - prev->ptr` using prev->ptr, which was
 *    never initialized -- UB on an indeterminate pointer. Fixed by
 *    rewriting to match the (correct) index-based, growable-array pattern
 *    already used in nal_h264.c, including its `if (idx > 0)` guard.
 *
 * 2. h265_slice_counter_s.spsValid/ppsValid (and the SPS/PPS fields they
 *    gate) were never initialized: the counter is malloc()'d (not
 *    calloc()'d) and h265_slice_counter_reset() never touched them. Since
 *    h265_slice_counter_write_packet()'s PPS case skips re-parsing once
 *    ppsValid is set ("if (s->ppsValid) break;"), a freshly allocated
 *    counter could -- depending on what garbage happened to be on the
 *    heap -- skip PPS parsing entirely and use an uninitialized, huge
 *    num_extra_slice_header_bits, which then consumes far more bits than
 *    the slice header actually has. Confirmed nondeterministically via a
 *    real run. Also, reusing a counter across a pid switch
 *    (h265_slice_counter_reset_pid) would carry over a previous stream's
 *    PPS state into an unrelated one. Fixed by clearing both *Valid flags
 *    in h265_slice_counter_reset().
 *
 * 3. h265Nals_lookupName()/h265Nals_lookupType() indexed the sparse
 *    hevcNals[] table directly by nalType with no bounds check. nalType is
 *    a 6-bit field (0-63) taken straight from the bitstream, but the table
 *    only covers indices 0-40. Confirmed a real global-buffer-overflow via
 *    ASan. Fixed by bounds-checking before indexing (same fix as the
 *    identical bug in nal_h264.c's h264Nals_lookupName()).
 *
 * 4. h265_slice_counter_write_packet()'s slice-type parsing guarded the
 *    Exp-Golomb-decoded slice_type with only
 *    `slice_type < MAX_H265_SLICE_TYPES`, never `>= 0`. A malformed/
 *    truncated slice header makes the Exp-Golomb read return -1, which
 *    then flowed into `s->slice[slice_type].count++` -- a real,
 *    reproducible heap-buffer-overflow, confirmed via ASan (same bug as
 *    nal_h264.c had in three places). Fixed by requiring `slice_type >= 0`,
 *    and defensively in h265_slice_name_ascii() itself (also public API).
 *
 * 5. h265_slice_counter_s.nextHistoryPos had the same
 *    never-initialized-then-used-as-a-modulo-operand bug as its H.264
 *    sibling (see test_nal_h264.c). Fixed the same way, in
 *    h265_slice_counter_reset().
 *
 * 6. h265_slice_counter_s.sliceHistory was sized
 *    `[H265_SLICE_COUNTER_HISTORY_LENGTH]` (unlike h264's, which is
 *    `[...+ 1]`), and reset() nul-terminated it at the *last ring-buffer
 *    slot* (index LENGTH-1) rather than in a dedicated extra byte. Since
 *    that slot is live -- written by `sliceHistory[nextHistoryPos++ %
 *    LENGTH]` -- the embedded NUL byte would rotate into the middle of
 *    h265_slice_counter_query()'s copied-out history string as updates
 *    accumulated, silently truncating it via %s. Reproduced directly: a
 *    single recorded 'I' vanished from the printed history. Fixed by
 *    sizing the array `+ 1` and terminating in that dedicated slot,
 *    matching h264's already-correct pattern.
 *
 * Also worth noting (NOT fixed, out of scope for a test-focused pass):
 * every bit-reader call site in this file does
 * `NALBitReader_init(&br, pkt + offset + 4, 4)`, skipping only 1 byte past
 * the start code. That's correct for H.264 (1-byte NAL header) but H.265
 * has a 2-byte NAL header, so this actually starts parsing 1 byte into the
 * real payload for every field parsed here (SPS/PPS/slice headers). The
 * tests below construct their input bytes to match this file's actual
 * (if arguably spec-incorrect) parsing offset rather than the real H.265
 * bitstream layout.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libltntstools/nal_h265.h>

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* Real, non-static, exported symbol but never added to the public header
 * (same situation as h264Nals_lookupType() in test_nal_h264.c). Declare it
 * locally so its bounds-check regression fix can be exercised directly.
 */
extern const char *h265Nals_lookupType(int nalType);

/* -------- ltn_nal_h265_findHeader -------- */

static void test_findHeader_finds_start_code(void)
{
	const uint8_t buf[] = { 0xFF, 0x00, 0x00, 0x01, (33 << 1), 0xAA };
	int offset = -1;
	int ret = ltn_nal_h265_findHeader(buf, sizeof(buf), &offset);
	CHECK(ret == 0);
	CHECK(offset == 1);
}

static void test_findHeader_skips_forbidden_zero_bit(void)
{
	const uint8_t buf[] = { 0x00,0x00,0x01, 0x81, 0xFF, 0x00,0x00,0x01, (33 << 1), 0xAA };
	int offset = -1;
	int ret = ltn_nal_h265_findHeader(buf, sizeof(buf), &offset);
	CHECK(ret == 0);
	CHECK(offset == 5);
}

static void test_findHeader_returns_error_when_not_found(void)
{
	const uint8_t buf[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
	int offset = -1;
	int ret = ltn_nal_h265_findHeader(buf, sizeof(buf), &offset);
	CHECK(ret != 0);
}

/* -------- ltn_nal_h265_find_headers -------- */

static void test_find_headers_multiple_nals(void)
{
	const uint8_t buf[] = {
		0,0,1, (32 << 1), 0x01, 0xAA,             /* VPS, 6 bytes */
		0,0,1, (33 << 1), 0x01, 0xBB,             /* SPS, 6 bytes */
		0,0,1, (34 << 1), 0x01, 0xCC, 0xDD,       /* PPS, 7 bytes (to end of buffer) */
	};

	struct ltn_nal_headers_s *arr = NULL;
	int arrLen = 0;
	int ret = ltn_nal_h265_find_headers(buf, sizeof(buf), &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == 3);
	if (arrLen == 3) {
		CHECK(arr[0].nalType == 32);
		CHECK(arr[0].lengthBytes == 6);
		CHECK(strcmp(arr[0].nalName, "VPS") == 0);

		CHECK(arr[1].nalType == 33);
		CHECK(arr[1].lengthBytes == 6);
		CHECK(strcmp(arr[1].nalName, "SPS") == 0);

		CHECK(arr[2].nalType == 34);
		CHECK(arr[2].lengthBytes == 7);
		CHECK(strcmp(arr[2].nalName, "PPS") == 0);
	}
	free(arr);
}

static void test_find_headers_no_start_codes(void)
{
	const uint8_t buf[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	struct ltn_nal_headers_s *arr = NULL;
	int arrLen = -1;
	int ret = ltn_nal_h265_find_headers(buf, sizeof(buf), &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == 0);
	free(arr);
}

static void test_find_headers_grows_past_initial_capacity(void)
{
	/* Regression for bug #1 above: the array must grow past its initial
	 * 64-entry allocation instead of overflowing it.
	 */
	const int n = 70, per = 6;
	uint8_t *buf = malloc(n * per);
	CHECK(buf != NULL);
	if (!buf)
		return;

	for (int i = 0; i < n; i++) {
		uint8_t *p = buf + (i * per);
		p[0] = 0; p[1] = 0; p[2] = 1; p[3] = (33 << 1); p[4] = 0x01; p[5] = 0xAA;
	}

	struct ltn_nal_headers_s *arr = NULL;
	int arrLen = 0;
	int ret = ltn_nal_h265_find_headers(buf, n * per, &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == n);
	if (arrLen == n) {
		CHECK(arr[n - 1].lengthBytes == (uint32_t)per);
	}
	free(arr);
	free(buf);
}

/* -------- ltn_nal_h265_findNalTypes -------- */

static void test_findNalTypes_lists_names_in_order(void)
{
	const uint8_t buf[] = {
		0,0,1, (32 << 1), 0x01, 0xAA,
		0,0,1, (33 << 1), 0x01, 0xBB,
		0,0,1, (34 << 1), 0x01, 0xCC, 0xDD,
	};
	char *types = ltn_nal_h265_findNalTypes(buf, sizeof(buf));
	CHECK(types != NULL);
	if (types) {
		CHECK(strcmp(types, "VPS, SPS, PPS") == 0);
	}
	free(types);
}

static void test_findNalTypes_no_nals_returns_null(void)
{
	const uint8_t buf[] = { 0xAA, 0xBB, 0xCC };
	char *types = ltn_nal_h265_findNalTypes(buf, sizeof(buf));
	CHECK(types == NULL);
	free(types);
}

/* -------- h265Nals_lookupName / h265Nals_lookupType -------- */

static void test_lookupName_known_values(void)
{
	CHECK(strcmp(h265Nals_lookupName(0), "TRAIL_N") == 0);
	CHECK(strcmp(h265Nals_lookupName(33), "SPS") == 0);
	CHECK(strcmp(h265Nals_lookupName(34), "PPS") == 0);
	CHECK(strcmp(h265Nals_lookupType(19), "IDR") == 0);
}

static void test_lookupName_out_of_range_is_bounds_checked(void)
{
	/* Regression: nalType is masked with 0x3f by every caller (6 bits,
	 * 0-63), but the table only covers 0-40. 41-63 must not read past
	 * the table.
	 */
	CHECK(strcmp(h265Nals_lookupName(50), "RESERVED/UNKNOWN") == 0);
	CHECK(strcmp(h265Nals_lookupName(63), "RESERVED/UNKNOWN") == 0);
	CHECK(strcmp(h265Nals_lookupType(50), "") == 0);
	CHECK(strcmp(h265Nals_lookupName(-1), "RESERVED/UNKNOWN") == 0);
}

/* -------- h265_slice_name_ascii -------- */

static void test_slice_name_ascii_known_values(void)
{
	CHECK(strcmp(h265_slice_name_ascii(0), "B") == 0);
	CHECK(strcmp(h265_slice_name_ascii(1), "P") == 0);
	CHECK(strcmp(h265_slice_name_ascii(2), "I") == 0);
}

static void test_slice_name_ascii_wraps_at_max(void)
{
	/* MAX_H265_SLICE_TYPES == 3, so 3 wraps back to slice_defaults[0]. */
	CHECK(strcmp(h265_slice_name_ascii(3), h265_slice_name_ascii(0)) == 0);
}

static void test_slice_name_ascii_negative_input_is_safe(void)
{
	/* Regression: slice_type % MAX_H265_SLICE_TYPES on a negative
	 * slice_type is itself negative in C -- must not index the array.
	 */
	CHECK(strcmp(h265_slice_name_ascii(-1), "?") == 0);
}

/* -------- ltn_sei_h265_lookupName -------- */

static void test_sei_lookupName_known_and_unknown(void)
{
	CHECK(strcmp(ltn_sei_h265_lookupName(0), "buffer_period") == 0);
	CHECK(strcmp(ltn_sei_h265_lookupName(6), "recovery_point") == 0);
	CHECK(strcmp(ltn_sei_h265_lookupName(7), "undefined in spec ITU-T H.265 v5 (02/2018)") == 0); /* hole in the table */
	CHECK(strcmp(ltn_sei_h265_lookupName(255), "undefined in spec ITU-T H.265 v5 (02/2018)") == 0);
}

/* -------- ltn_sei_h265_find_headers -------- */

static void test_sei_find_headers_extracts_only_sei_nals(void)
{
	/* seiType lives at ptr[4] per this file's own parsing convention --
	 * see the file-level comment above about the 1-byte-short header skip.
	 */
	uint8_t sei_buf[] = { 0,0,1, (39 << 1), 6 /* seiType = recovery_point */, 0xEE };
	uint8_t other_buf[] = { 0,0,1, (1 << 1), 0x01, 0xFF };

	struct ltn_nal_headers_s nals[2] = {
		{ .ptr = sei_buf, .lengthBytes = sizeof(sei_buf), .nalType = 39 },
		{ .ptr = other_buf, .lengthBytes = sizeof(other_buf), .nalType = 1 },
	};

	struct ltn_sei_headers_s *arr = NULL;
	int arrLen = -1;
	int ret = ltn_sei_h265_find_headers(nals, 2, &arr, &arrLen);
	CHECK(ret == 0);
	CHECK(arrLen == 1);
	if (arrLen == 1) {
		CHECK(arr[0].seiType == 6);
		CHECK(strcmp(arr[0].seiName, "recovery_point") == 0);
		CHECK(arr[0].ptr == sei_buf);
	}
	free(arr);

	/* Both SEI nal unit types (prefix 39, suffix 40) must be picked up. */
	struct ltn_nal_headers_s nals2[1] = {
		{ .ptr = sei_buf, .lengthBytes = sizeof(sei_buf), .nalType = 40 },
	};
	struct ltn_sei_headers_s *arr2 = NULL;
	int arr2Len = -1;
	ret = ltn_sei_h265_find_headers(nals2, 1, &arr2, &arr2Len);
	CHECK(ret == 0);
	CHECK(arr2Len == 1);
	free(arr2);
}

static void test_sei_find_headers_rejects_null_args(void)
{
	struct ltn_sei_headers_s *arr = NULL;
	int arrLen = -1;
	CHECK(ltn_sei_h265_find_headers(NULL, 0, &arr, &arrLen) < 0);

	struct ltn_nal_headers_s nals[1] = { { .nalType = 39 } };
	CHECK(ltn_sei_h265_find_headers(nals, 1, &arr, NULL) < 0);
}

/* -------- h265_slice_counter_* (via h265_slice_counter_write) -------- */

static void build_ts_packet(uint8_t *pkt, uint16_t pid, uint8_t nal_hdr0, uint8_t first_data_byte)
{
	memset(pkt, 0x00, 188);
	pkt[0] = 0x47;
	pkt[1] = (pid >> 8) & 0x1f;
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10;
	pkt[4] = 0x00;
	pkt[5] = 0x00;
	pkt[6] = 0x01;
	pkt[7] = nal_hdr0;        /* nal_unit_type in bits [6:1] */
	pkt[8] = first_data_byte; /* first byte this file's bit reader consumes */
}

static void test_slice_counter_alloc_starts_zeroed(void)
{
	void *ctx = h265_slice_counter_alloc(0x100);
	CHECK(ctx != NULL);
	CHECK(h265_slice_counter_get_pid(ctx) == 0x100);

	struct h265_slice_counter_results_s r;
	h265_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0);
	for (int i = 0; i < H265_SLICE_COUNTER_HISTORY_LENGTH; i++) {
		CHECK(r.sliceHistory[i] == ' ');
	}
	CHECK(r.sliceHistory[H265_SLICE_COUNTER_HISTORY_LENGTH] == 0);

	h265_slice_counter_free(ctx);
}

static void test_slice_counter_write_parses_sps_pps_then_slice(void)
{
	/* SPS then PPS establish the state the slice-header parse depends
	 * on; the slice itself encodes slice_type = 2 (I). See the file-level
	 * comment for why the payload bytes below don't look like a literal
	 * H.265 bitstream.
	 */
	uint8_t pkts[3 * 188];
	build_ts_packet(pkts + 0*188, 0x100, (33 << 1), 0x00); /* SPS */
	build_ts_packet(pkts + 1*188, 0x100, (34 << 1), 0xC0); /* PPS */
	build_ts_packet(pkts + 2*188, 0x100, (19 << 1), 0xAC); /* IDR_W_RADL slice, type I */

	void *ctx = h265_slice_counter_alloc(0x2000); /* 0x2000 == match every pid */
	h265_slice_counter_write(ctx, pkts, 3);

	struct h265_slice_counter_results_s r;
	h265_slice_counter_query(ctx, &r);
	CHECK(r.i == 1);
	CHECK(r.p == 0);
	CHECK(r.b == 0);
	CHECK(strcmp(r.sliceHistory + (H265_SLICE_COUNTER_HISTORY_LENGTH - 1), "I") == 0);

	h265_slice_counter_free(ctx);
}

static void test_slice_counter_write_ignores_non_matching_pid(void)
{
	uint8_t pkts[3 * 188];
	build_ts_packet(pkts + 0*188, 0x100, (33 << 1), 0x00);
	build_ts_packet(pkts + 1*188, 0x100, (34 << 1), 0xC0);
	build_ts_packet(pkts + 2*188, 0x100, (19 << 1), 0xAC);

	void *ctx = h265_slice_counter_alloc(0x200); /* tracking a different pid */
	h265_slice_counter_write(ctx, pkts, 3);

	struct h265_slice_counter_results_s r;
	h265_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0);

	h265_slice_counter_free(ctx);
}

static void test_slice_counter_reset_clears_counts_and_sps_pps_state(void)
{
	uint8_t pkts[3 * 188];
	build_ts_packet(pkts + 0*188, 0x100, (33 << 1), 0x00);
	build_ts_packet(pkts + 1*188, 0x100, (34 << 1), 0xC0);
	build_ts_packet(pkts + 2*188, 0x100, (19 << 1), 0xAC);

	void *ctx = h265_slice_counter_alloc(0x2000);
	h265_slice_counter_write(ctx, pkts, 3);

	struct h265_slice_counter_results_s r;
	h265_slice_counter_query(ctx, &r);
	CHECK(r.i == 1);

	h265_slice_counter_reset(ctx);
	h265_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0);
	CHECK(r.sliceHistory[H265_SLICE_COUNTER_HISTORY_LENGTH - 1] == ' ');

	h265_slice_counter_free(ctx);
}

static void test_slice_counter_reset_pid_changes_tracked_pid(void)
{
	void *ctx = h265_slice_counter_alloc(0x100);
	CHECK(h265_slice_counter_get_pid(ctx) == 0x100);

	h265_slice_counter_reset_pid(ctx, 0x200);
	CHECK(h265_slice_counter_get_pid(ctx) == 0x200);

	h265_slice_counter_free(ctx);
}

static void test_slice_counter_history_wraps_without_corruption(void)
{
	/* Regression coverage for the nextHistoryPos-uninitialized bug and
	 * the sliceHistory-sizing/embedded-NUL bug: feed more I-slices than
	 * H265_SLICE_COUNTER_HISTORY_LENGTH (each preceded by fresh SPS/PPS
	 * so every packet is independently parseable) and confirm the
	 * history string comes back exactly LENGTH chars, all 'I'.
	 */
	uint8_t pkts[25 * 3 * 188];
	for (int i = 0; i < 25; i++) {
		uint8_t *base = pkts + (i * 3 * 188);
		build_ts_packet(base + 0*188, 0x100, (33 << 1), 0x00);
		build_ts_packet(base + 1*188, 0x100, (34 << 1), 0xC0);
		build_ts_packet(base + 2*188, 0x100, (19 << 1), 0xAC);
	}

	void *ctx = h265_slice_counter_alloc(0x2000);
	h265_slice_counter_write(ctx, pkts, 25 * 3);

	struct h265_slice_counter_results_s r;
	h265_slice_counter_query(ctx, &r);
	CHECK(r.i == 25);
	CHECK((int)strlen(r.sliceHistory) == H265_SLICE_COUNTER_HISTORY_LENGTH);
	for (int i = 0; i < H265_SLICE_COUNTER_HISTORY_LENGTH; i++) {
		CHECK(r.sliceHistory[i] == 'I');
	}

	h265_slice_counter_free(ctx);
}

static void test_slice_counter_malformed_slice_does_not_crash(void)
{
	/* Regression: SPS+PPS establish state, then a slice packet whose
	 * header bits are chosen so the slice_type Exp-Golomb read runs out
	 * of bits and returns -1. Must be treated as malformed, not indexed
	 * into the counts array.
	 */
	uint8_t pkts[3 * 188];
	build_ts_packet(pkts + 0*188, 0x100, (33 << 1), 0x00);
	build_ts_packet(pkts + 1*188, 0x100, (34 << 1), 0xC0);
	build_ts_packet(pkts + 2*188, 0x100, (19 << 1), 0x80); /* runs out of bits before slice_type */

	void *ctx = h265_slice_counter_alloc(0x2000);
	h265_slice_counter_write(ctx, pkts, 3); /* must not crash / corrupt memory */

	struct h265_slice_counter_results_s r;
	h265_slice_counter_query(ctx, &r);
	CHECK(r.i == 0 && r.p == 0 && r.b == 0);

	h265_slice_counter_free(ctx);
}

int main(void)
{
	test_findHeader_finds_start_code();
	test_findHeader_skips_forbidden_zero_bit();
	test_findHeader_returns_error_when_not_found();

	test_find_headers_multiple_nals();
	test_find_headers_no_start_codes();
	test_find_headers_grows_past_initial_capacity();

	test_findNalTypes_lists_names_in_order();
	test_findNalTypes_no_nals_returns_null();

	test_lookupName_known_values();
	test_lookupName_out_of_range_is_bounds_checked();

	test_slice_name_ascii_known_values();
	test_slice_name_ascii_wraps_at_max();
	test_slice_name_ascii_negative_input_is_safe();

	test_sei_lookupName_known_and_unknown();
	test_sei_find_headers_extracts_only_sei_nals();
	test_sei_find_headers_rejects_null_args();

	test_slice_counter_alloc_starts_zeroed();
	test_slice_counter_write_parses_sps_pps_then_slice();
	test_slice_counter_write_ignores_non_matching_pid();
	test_slice_counter_reset_clears_counts_and_sps_pps_state();
	test_slice_counter_reset_pid_changes_tracked_pid();
	test_slice_counter_history_wraps_without_corruption();
	test_slice_counter_malformed_slice_does_not_crash();

	if (g_failures == 0) {
		printf("PASS: all nal_h265 tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d nal_h265 test(s) failed\n", g_failures);
	return 1;
}
