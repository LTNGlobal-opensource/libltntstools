/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/ts.c / src/libltntstools/ts.h.
 * Builds directly against ../src/ts.c, no other library dependencies
 * (ts.c only needs libc; the streammodel.h include it pulls in is
 * declarations-only, nothing from it is called).
 *
 * NOTE: ltntstools_file_estimate_bitrate() is declared in ts.h but has no
 * implementation anywhere in the repository (confirmed via grep across
 * src/ and rust/) and has no callers either. It cannot be linked, so it is
 * intentionally not exercised here.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

#include "libltntstools/ts.h"

/* Some regression tests below need to prove a fixed out-of-bounds read stays
 * fixed, even on a build without ASan. A plain heap/stack buffer won't do
 * that reliably (an over-read might land on unrelated-but-mapped memory and
 * silently "succeed"). Instead we mmap the buffer so it butts up against an
 * unmapped/PROT_NONE guard page: any read even one byte past the requested
 * length faults immediately (SIGSEGV / SIGBUS) and the test binary crashes,
 * making a reintroduced over-read impossible to miss. */
struct guarded_buf {
	uint8_t *ptr;    /* usable buffer, exactly `len` bytes, guard page follows */
	uint8_t *base;   /* mmap() base, needed to unmap */
	size_t   maplen; /* total mmap() length */
};

static int guarded_buf_alloc(struct guarded_buf *g, size_t len)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	size_t datapages = (len + (size_t)pagesize - 1) / (size_t)pagesize;
	if (datapages == 0)
		datapages = 1;
	g->maplen = (datapages + 1) * (size_t)pagesize;

	g->base = mmap(NULL, g->maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g->base == MAP_FAILED)
		return -1;

	if (mprotect(g->base + datapages * (size_t)pagesize, (size_t)pagesize, PROT_NONE) != 0) {
		munmap(g->base, g->maplen);
		return -1;
	}

	/* Butt the end of the caller's region against the guard page. */
	g->ptr = g->base + (datapages * (size_t)pagesize) - len;
	return 0;
}

static void guarded_buf_free(struct guarded_buf *g)
{
	munmap(g->base, g->maplen);
}

/* Not declared in ts.h (internal helper), but it's a real exported symbol
 * in ts.c -- prototype it ourselves so we can test it directly. */
extern uint64_t ltntstools_pcrToScr(const uint8_t *ptr, int len);

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

static void set_header(uint8_t *pkt, uint16_t pid, int tei, int pusi, int tp,
	uint8_t tsc, uint8_t afc, uint8_t cc)
{
	pkt[0] = 0x47;
	pkt[1] = (tei ? 0x80 : 0) | (pusi ? 0x40 : 0) | (tp ? 0x20 : 0) | ((pid >> 8) & 0x1f);
	pkt[2] = pid & 0xff;
	pkt[3] = ((tsc & 0x3) << 6) | ((afc & 0x3) << 4) | (cc & 0x0f);
}

/* -------- bit-field accessors -------- */

static void test_sync_present(void)
{
	uint8_t pkt[188] = { 0 };
	pkt[0] = 0x47;
	CHECK(ltntstools_sync_present(pkt) == 1);
	pkt[0] = 0x46;
	CHECK(ltntstools_sync_present(pkt) == 0);
}

static void test_tei_set(void)
{
	uint8_t pkt[188];
	set_header(pkt, 0x100, 1, 0, 0, 0, 0, 0);
	CHECK(ltntstools_tei_set(pkt) == 1);
	set_header(pkt, 0x100, 0, 0, 0, 0, 0, 0);
	CHECK(ltntstools_tei_set(pkt) == 0);
}

static void test_pusi(void)
{
	uint8_t pkt[188];
	set_header(pkt, 0x100, 0, 1, 0, 0, 0, 0);
	CHECK(ltntstools_payload_unit_start_indicator(pkt) == 1);
	set_header(pkt, 0x100, 0, 0, 0, 0, 0, 0);
	CHECK(ltntstools_payload_unit_start_indicator(pkt) == 0);
}

static void test_transport_priority(void)
{
	uint8_t pkt[188];
	set_header(pkt, 0x100, 0, 0, 1, 0, 0, 0);
	CHECK(ltntstools_transport_priority(pkt) == 1);
	set_header(pkt, 0x100, 0, 0, 0, 0, 0, 0);
	CHECK(ltntstools_transport_priority(pkt) == 0);
}

static void test_pid(void)
{
	uint8_t pkt[188];
	set_header(pkt, 0x0000, 0, 0, 0, 0, 0, 0);
	CHECK(ltntstools_pid(pkt) == 0x0000);

	set_header(pkt, 0x1fff, 0, 0, 0, 0, 0, 0);
	CHECK(ltntstools_pid(pkt) == 0x1fff);

	set_header(pkt, 0x0031, 0, 0, 0, 0, 0, 0);
	CHECK(ltntstools_pid(pkt) == 0x0031);

	/* Upper 3 bits of byte1 (tei/pusi/tp) must not leak into the PID. */
	set_header(pkt, 0x0031, 1, 1, 1, 0, 0, 0);
	CHECK(ltntstools_pid(pkt) == 0x0031);
}

static void test_transport_scrambling_control(void)
{
	uint8_t pkt[188];
	for (uint8_t tsc = 0; tsc <= 3; tsc++) {
		set_header(pkt, 0x100, 0, 0, 0, tsc, 0, 0);
		CHECK(ltntstools_transport_scrambling_control(pkt) == tsc);
	}
}

static void test_adaption_field_control(void)
{
	uint8_t pkt[188];
	for (uint8_t afc = 0; afc <= 3; afc++) {
		set_header(pkt, 0x100, 0, 0, 0, 0, afc, 0);
		CHECK(ltntstools_adaption_field_control(pkt) == afc);
	}
}

static void test_has_adaption(void)
{
	uint8_t pkt[188];
	set_header(pkt, 0x100, 0, 0, 0, 0, 0, 0); CHECK(ltntstools_has_adaption(pkt) == 0);
	set_header(pkt, 0x100, 0, 0, 0, 0, 1, 0); CHECK(ltntstools_has_adaption(pkt) == 0);
	set_header(pkt, 0x100, 0, 0, 0, 0, 2, 0); CHECK(ltntstools_has_adaption(pkt) == 1);
	set_header(pkt, 0x100, 0, 0, 0, 0, 3, 0); CHECK(ltntstools_has_adaption(pkt) == 1);
}

static void test_adaption_field_length(void)
{
	uint8_t pkt[188] = { 0 };
	set_header(pkt, 0x100, 0, 0, 0, 0, 2, 0);
	pkt[4] = 42;
	CHECK(ltntstools_adaption_field_length(pkt) == 42);
}

static void test_continuity_counter(void)
{
	uint8_t pkt[188];
	for (uint8_t cc = 0; cc <= 15; cc++) {
		set_header(pkt, 0x100, 0, 0, 0, 0, 0, cc);
		CHECK(ltntstools_continuity_counter(pkt) == cc);
	}
}

/* -------- PCR pack / unpack -------- */

static void test_pcrToScr_known_values(void)
{
	/* base=1, ext=5 -> 1*300+5 = 305 */
	uint8_t a[6] = { 0x00, 0x00, 0x00, 0x00, 0x80, 0x05 };
	CHECK(ltntstools_pcrToScr(a, 6) == 305);

	/* base=0x123456, ext=0x0a5 -> 0x123456*300+0x0a5 = 357913965 */
	uint8_t b[6] = { 0x00, 0x09, 0x1a, 0x2b, 0x00, 0xa5 };
	CHECK(ltntstools_pcrToScr(b, 6) == 357913965ULL);

	/* max 33-bit base (0x1ffffffff) + max legal ext (299, i.e. 0-299 valid
	 * per the 90kHz/27MHz 300:1 ratio -- not the full 9-bit range) -> MAX_SCR_VALUE - 1 */
	uint8_t c[6] = { 0xff, 0xff, 0xff, 0xff, 0x81, 0x2b };
	CHECK(ltntstools_pcrToScr(c, 6) == MAX_SCR_VALUE - 1);
}

static void test_pcr_packTo_roundtrip(void)
{
	uint64_t values[] = { 0, 1, 299, 300, 301, 123456789ULL, MAX_SCR_VALUE - 1 };
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		uint8_t buf[6];
		ltntstools_pcr_packTo(buf, sizeof(buf), values[i]);
		uint64_t decoded = ltntstools_pcrToScr(buf, 6);
		if (decoded != values[i]) {
			fprintf(stderr, "FAIL: roundtrip mismatch for %llu -> got %llu\n",
				(unsigned long long)values[i], (unsigned long long)decoded);
			g_failures++;
		}
	}
}

/* -------- ltntstools_scr() -------- */

static void test_scr_rejects_no_sync(void)
{
	uint8_t pkt[188] = { 0 };
	pkt[0] = 0x46; /* bad sync */
	uint64_t scr;
	CHECK(ltntstools_scr(pkt, &scr) < 0);
}

static void test_scr_rejects_no_adaptation(void)
{
	uint8_t pkt[188] = { 0 };
	set_header(pkt, 0x100, 0, 0, 0, 0, 1 /* payload only */, 0);
	uint64_t scr;
	CHECK(ltntstools_scr(pkt, &scr) < 0);
}

static void test_scr_rejects_adaptation_length_zero(void)
{
	uint8_t pkt[188] = { 0 };
	set_header(pkt, 0x100, 0, 0, 0, 0, 2, 0);
	pkt[4] = 0; /* adaptation_field_length == 0 */
	uint64_t scr;
	CHECK(ltntstools_scr(pkt, &scr) < 0);
}

static void test_scr_rejects_pcr_flag_not_set(void)
{
	uint8_t pkt[188] = { 0 };
	set_header(pkt, 0x100, 0, 0, 0, 0, 2, 0);
	pkt[4] = 183;
	pkt[5] = 0x00; /* PCR_flag not set */
	uint64_t scr;
	CHECK(ltntstools_scr(pkt, &scr) < 0);
}

static void test_scr_extracts_valid_pcr(void)
{
	uint8_t pkt[188];
	uint8_t cc = 0;
	CHECK(ltntstools_generatePCROnlyPacket(pkt, sizeof(pkt), 0x100, &cc, 987654321ULL) == 0);

	uint64_t scr = 0;
	CHECK(ltntstools_scr(pkt, &scr) == 0);
	CHECK(scr == 987654321ULL);
}

/* -------- scr_diff / scr_add / pts_diff -------- */

static void test_scr_diff_no_wrap(void)
{
	CHECK(ltntstools_scr_diff(1000, 5000) == 4000);
}

static void test_scr_diff_wrap(void)
{
	int64_t from = MAX_SCR_VALUE - 100;
	int64_t to = 50; /* < 5*27000000, triggers wrap path */
	int64_t expect = (MAX_SCR_VALUE - from) + to;
	CHECK(ltntstools_scr_diff(from, to) == expect);
	CHECK(expect == 150);
}

static void test_scr_add_no_wrap(void)
{
	CHECK(ltntstools_scr_add(1000, 500) == 1500);
	CHECK(ltntstools_scr_add(1000, -500) == 500);
}

static void test_scr_add_overflow_wraps(void)
{
	int64_t result = ltntstools_scr_add(MAX_SCR_VALUE - 10, 20);
	CHECK(result == 10);
}

static void test_scr_add_negative_wraps(void)
{
	int64_t result = ltntstools_scr_add(10, -20);
	CHECK(result == MAX_SCR_VALUE - 10);
}

static void test_pts_diff_no_wrap(void)
{
	CHECK(ltntstools_pts_diff(1000, 5000) == 4000);
}

static void test_pts_diff_wrap(void)
{
	int64_t from = MAX_PTS_VALUE - 100;
	int64_t to = 50;
	int64_t expect = (MAX_PTS_VALUE - from) + to;
	CHECK(ltntstools_pts_diff(from, to) == expect);
	CHECK(expect == 150);
}

/* -------- contains_pes_header / reverse -------- */

static void test_contains_pes_header_found(void)
{
	uint8_t buf[32] = { 0 };
	buf[10] = 0x00; buf[11] = 0x00; buf[12] = 0x01; buf[13] = 0xe0;
	CHECK(ltntstools_contains_pes_header(buf, sizeof(buf)) == 10);
}

static void test_contains_pes_header_not_found(void)
{
	uint8_t buf[32];
	memset(buf, 0xAA, sizeof(buf));
	CHECK(ltntstools_contains_pes_header(buf, sizeof(buf)) < 0);
}

static void test_contains_pes_header_forward_vs_reverse(void)
{
	uint8_t buf[32] = { 0 };
	buf[4] = 0x00; buf[5] = 0x00; buf[6] = 0x01;   /* first occurrence */
	buf[20] = 0x00; buf[21] = 0x00; buf[22] = 0x01; /* second occurrence */

	CHECK(ltntstools_contains_pes_header(buf, sizeof(buf)) == 4);
	CHECK(ltntstools_contains_pes_header_reverse(buf, sizeof(buf)) == 20);
}

static void test_contains_pes_header_short_buffer_safe(void)
{
	uint8_t buf[2] = { 0x00, 0x00 };
	CHECK(ltntstools_contains_pes_header(buf, 0) < 0);
	CHECK(ltntstools_contains_pes_header(buf, 2) < 0);
	CHECK(ltntstools_contains_pes_header_reverse(buf, 0) < 0);
	CHECK(ltntstools_contains_pes_header_reverse(buf, 2) < 0);
}

/* -------- get_section_tableid -------- */

static void test_get_section_tableid_no_adaptation(void)
{
	uint8_t pkt[188] = { 0 };
	set_header(pkt, 0x100, 0, 0, 0, 0, 1 /* payload only */, 0);
	pkt[5] = 0x42;
	CHECK(ltntstools_get_section_tableid(pkt) == 0x42);
}

static void test_get_section_tableid_with_adaptation(void)
{
	uint8_t pkt[188] = { 0 };
	set_header(pkt, 0x100, 0, 0, 0, 0, 3 /* adaptation + payload */, 0);
	pkt[4] = 10; /* adaptation_field_length */
	pkt[5 + 1 + 10] = 0x77;
	CHECK(ltntstools_get_section_tableid(pkt) == 0x77);
}

/* -------- ES payload type classification -------- */

static void test_is_ESPayloadType_Video_spotcheck(void)
{
	CHECK(ltntstools_is_ESPayloadType_Video(0x1B) == 1); /* H.264 */
	CHECK(ltntstools_is_ESPayloadType_Video(0x24) == 1); /* HEVC */
	CHECK(ltntstools_is_ESPayloadType_Video(0xd2) == 1); /* AV1 */
	CHECK(ltntstools_is_ESPayloadType_Video(0x03) == 0); /* audio, not video */
	CHECK(ltntstools_is_ESPayloadType_Video(0x05) == 0); /* private section */
}

static void test_is_ESPayloadType_Audio_spotcheck(void)
{
	CHECK(ltntstools_is_ESPayloadType_Audio(0x81) == 1); /* AC-3 */
	CHECK(ltntstools_is_ESPayloadType_Audio(0x0F) == 1); /* AAC/ADTS */
	CHECK(ltntstools_is_ESPayloadType_Audio(0x1B) == 0); /* video, not audio */
	CHECK(ltntstools_is_ESPayloadType_Audio(0x05) == 0); /* private section */
}

/* Documented invariant: never returns NULL, for any possible input byte. */
static void test_GetESPayloadTypeDescription_never_null(void)
{
	for (int t = 0; t <= 0xff; t++) {
		if (ltntstools_GetESPayloadTypeDescription((uint8_t)t) == NULL) {
			fprintf(stderr, "FAIL: GetESPayloadTypeDescription(0x%02x) returned NULL\n", t);
			g_failures++;
		}
	}
	CHECK(strcmp(ltntstools_GetESPayloadTypeDescription(0x1B), "AVC/H.264 Video") == 0);
	CHECK(strcmp(ltntstools_GetESPayloadTypeDescription(0x00), "Reserved") == 0);
	CHECK(strcmp(ltntstools_GetESPayloadTypeDescription(0xFF), "User Private") == 0);
}

/* -------- generateNullPacket / findSyncPosition -------- */

static void test_generateNullPacket(void)
{
	uint8_t pkt[188];
	ltntstools_generateNullPacket(pkt);

	CHECK(pkt[0] == 0x47);
	CHECK(ltntstools_pid(pkt) == 0x1fff);
	CHECK(ltntstools_adaption_field_control(pkt) == 1); /* payload only */
	for (int i = 4; i < 188; i++) {
		if (pkt[i] != 0xff) {
			fprintf(stderr, "FAIL: null packet stuffing byte %d != 0xff\n", i);
			g_failures++;
			break;
		}
	}
}

static void test_findSyncPosition_aligned(void)
{
	uint8_t buf[3 * 188] = { 0 };
	buf[0] = 0x47;
	buf[188] = 0x47;
	buf[376] = 0x47;
	CHECK(ltntstools_findSyncPosition(buf, sizeof(buf)) == 0);
}

static void test_findSyncPosition_offset(void)
{
	uint8_t buf[5 + 3 * 188] = { 0 };
	buf[5] = 0x47;
	buf[5 + 188] = 0x47;
	buf[5 + 376] = 0x47;
	CHECK(ltntstools_findSyncPosition(buf, sizeof(buf)) == 5);
}

static void test_findSyncPosition_too_short(void)
{
	uint8_t buf[3 * 188 - 1] = { 0 };
	CHECK(ltntstools_findSyncPosition(buf, sizeof(buf)) < 0);
}

static void test_findSyncPosition_none_found(void)
{
	uint8_t buf[3 * 188] = { 0 };
	CHECK(ltntstools_findSyncPosition(buf, sizeof(buf)) < 0);
}

/* -------- queryPCRs / queryPCR_pid / pcr_position_append -------- */

static void build_pcr_packet(uint8_t *pkt, uint16_t pid, uint64_t pcr)
{
	uint8_t cc = 0;
	ltntstools_generatePCROnlyPacket(pkt, 188, pid, &cc, pcr);
}

static void test_queryPCRs_finds_all_pcr_bearing_packets(void)
{
	uint8_t buf[3 * 188];
	build_pcr_packet(buf, 0x100, 1000);

	/* Plain payload-only packet, no PCR. */
	memset(buf + 188, 0xff, 188);
	set_header(buf + 188, 0x200, 0, 0, 0, 0, 1, 0);

	build_pcr_packet(buf + 376, 0x300, 2000);

	struct ltntstools_pcr_position_s *arr = NULL;
	int arrLen = 0;
	CHECK(ltntstools_queryPCRs(buf, sizeof(buf), 0, &arr, &arrLen) == 0);
	CHECK(arrLen == 2);
	if (arrLen == 2) {
		CHECK(arr[0].pid == 0x100);
		CHECK(arr[0].pcr == 1000);
		CHECK(arr[0].offset == 0);
		CHECK(arr[1].pid == 0x300);
		CHECK(arr[1].pcr == 2000);
		CHECK(arr[1].offset == 376);
	}
	free(arr);
}

static void test_queryPCR_pid_aligned_finds_match(void)
{
	uint8_t buf[2 * 188];
	build_pcr_packet(buf, 0x100, 5555);
	build_pcr_packet(buf + 188, 0x200, 6666);

	struct ltntstools_pcr_position_s pos;
	ltntstools_pcr_position_reset(&pos);

	CHECK(ltntstools_queryPCR_pid(buf, sizeof(buf), &pos, 0x200, 1 /* pktAligned */) == 0);
	CHECK(pos.pid == 0x200);
	CHECK(pos.pcr == 6666);
	CHECK(pos.offset == 188);
}

/* ltntstools_queryPCR_pid() with pktAligned=0 must locate the true sync
 * offset (via ltntstools_findSyncPosition(), which requires >= 3*188 bytes
 * to detect alignment) and scan from there, not from byte 0. */
static void test_queryPCR_pid_unaligned_offset(void)
{
	uint8_t buf[5 + 3 * 188];
	memset(buf, 0xAA, 5); /* leading junk before alignment */
	build_pcr_packet(buf + 5, 0x100, 1111);
	build_pcr_packet(buf + 5 + 188, 0x200, 2222);
	build_pcr_packet(buf + 5 + 376, 0x300, 3333);

	struct ltntstools_pcr_position_s pos;
	ltntstools_pcr_position_reset(&pos);

	int ret = ltntstools_queryPCR_pid(buf, sizeof(buf), &pos, 0x200, 0 /* NOT pre-aligned */);
	CHECK(ret == 0);
	CHECK(pos.pid == 0x200);
	CHECK(pos.pcr == 2222);
	CHECK(pos.offset == (uint64_t)(5 + 188));
}

static void test_queryPCR_pid_no_match_returns_error(void)
{
	uint8_t buf[188];
	build_pcr_packet(buf, 0x100, 1234);

	struct ltntstools_pcr_position_s pos;
	ltntstools_pcr_position_reset(&pos);

	CHECK(ltntstools_queryPCR_pid(buf, sizeof(buf), &pos, 0x999, 1) < 0);
}

static void test_pcr_position_append_grows_array(void)
{
	struct ltntstools_pcr_position_s *arr = NULL;
	int len = 0;

	for (int i = 0; i < 3; i++) {
		struct ltntstools_pcr_position_s p = { .pcr = i * 100, .offset = (uint64_t)i, .pid = (uint16_t)i };
		CHECK(ltntstools_pcr_position_append(&arr, &len, &p) == 0);
	}
	CHECK(len == 3);
	CHECK(arr[0].pcr == 0);
	CHECK(arr[1].pcr == 100);
	CHECK(arr[2].pcr == 200);
	free(arr);
}

/* Regression test: with pktAligned=1 and a small lengthBytes, the old bound
 * `i < lengthBytes - offset` let the loop enter with a buffer far shorter
 * than 188 bytes, and the RTP-header probe `pkt[12]` then read well past the
 * end of the buffer. The fixed bound (`i <= lengthBytes - 188`) must refuse
 * to enter the loop at all here. Buffer is mmap-guarded so any over-read
 * faults immediately instead of silently succeeding. */
static void test_queryPCR_pid_tiny_aligned_buffer_no_oob_read(void)
{
	struct guarded_buf g;
	CHECK(guarded_buf_alloc(&g, 5) == 0);
	memset(g.ptr, 0x47, 5);

	struct ltntstools_pcr_position_s pos;
	ltntstools_pcr_position_reset(&pos);

	CHECK(ltntstools_queryPCR_pid(g.ptr, 5, &pos, 0x100, 1 /* pktAligned */) < 0);

	guarded_buf_free(&g);
}

/* Regression test: a buffer holding N full packets plus a short trailing
 * remainder (not a full 188 bytes) must not have that remainder inspected.
 * The old bound would enter one extra iteration and read the RTP-header
 * probe / ltntstools_scr() fields straight past the buffer end. */
static void test_queryPCRs_trailing_partial_packet_no_oob_read(void)
{
	const int fullPackets = 3;
	const int trailing = 5; /* far short of another 188-byte packet */
	size_t len = fullPackets * 188 + trailing;

	struct guarded_buf g;
	CHECK(guarded_buf_alloc(&g, len) == 0);

	memset(g.ptr, 0xff, len);
	for (int i = 0; i < fullPackets; i++) {
		set_header(g.ptr + (i * 188), 0x100 + i, 0, 0, 0, 0, 1, 0);
	}
	/* Trailing bytes intentionally left as short, unaligned junk. */

	struct ltntstools_pcr_position_s *arr = NULL;
	int arrLen = 0;
	CHECK(ltntstools_queryPCRs(g.ptr, (int)len, 0, &arr, &arrLen) == 0);
	CHECK(arrLen == 0); /* none of the packets built above carry a PCR */
	free(arr);

	guarded_buf_free(&g);
}

/* Same trailing-remainder scenario via ltntstools_queryPCR_pid(), forcing a
 * full scan (no match) with pktAligned=0 so ltntstools_findSyncPosition()
 * establishes a non-zero offset first. */
static void test_queryPCR_pid_trailing_partial_packet_no_oob_read(void)
{
	const int fullPackets = 3;
	const int trailing = 5;
	size_t len = fullPackets * 188 + trailing;

	struct guarded_buf g;
	CHECK(guarded_buf_alloc(&g, len) == 0);

	memset(g.ptr, 0xff, len);
	for (int i = 0; i < fullPackets; i++) {
		set_header(g.ptr + (i * 188), 0x100 + i, 0, 0, 0, 0, 1, 0);
	}

	struct ltntstools_pcr_position_s pos;
	ltntstools_pcr_position_reset(&pos);

	CHECK(ltntstools_queryPCR_pid(g.ptr, (int)len, &pos, 0x999 /* no match */, 0) < 0);

	guarded_buf_free(&g);
}

/* -------- generate / update / verify 64-bit counter packets -------- */

static void test_generate_verify_roundtrip(void)
{
	uint8_t pkt[188];
	uint8_t cc = 0;
	CHECK(ltntstools_generatePacketWith64bCounter(pkt, sizeof(pkt), 0x40, &cc, 100) == 0);
	CHECK(cc == 1); /* cc post-incremented */

	uint64_t current = 0;
	CHECK(ltntstools_verifyPacketWith64bCounter(pkt, sizeof(pkt), 0x40, 99, &current) == 0);
	CHECK(current == 100);
}

static void test_verify_detects_wrong_pid(void)
{
	uint8_t pkt[188];
	uint8_t cc = 0;
	ltntstools_generatePacketWith64bCounter(pkt, sizeof(pkt), 0x40, &cc, 100);

	uint64_t current = 0;
	CHECK(ltntstools_verifyPacketWith64bCounter(pkt, sizeof(pkt), 0x41, 99, &current) < 0);
}

static void test_verify_detects_counter_mismatch(void)
{
	uint8_t pkt[188];
	uint8_t cc = 0;
	ltntstools_generatePacketWith64bCounter(pkt, sizeof(pkt), 0x40, &cc, 100);

	uint64_t current = 0;
	CHECK(ltntstools_verifyPacketWith64bCounter(pkt, sizeof(pkt), 0x40, 50, &current) < 0);
}

static void test_verify_detects_payload_corruption(void)
{
	uint8_t pkt[188];
	uint8_t cc = 0;
	ltntstools_generatePacketWith64bCounter(pkt, sizeof(pkt), 0x40, &cc, 100);

	pkt[50] ^= 0xff; /* corrupt a stuffing byte */

	uint64_t current = 0;
	CHECK(ltntstools_verifyPacketWith64bCounter(pkt, sizeof(pkt), 0x40, 99, &current) < 0);
}

static void test_update_then_verify(void)
{
	uint8_t pkt[188];
	uint8_t cc = 0;
	CHECK(ltntstools_generatePacketWith64bCounter(pkt, sizeof(pkt), 0x40, &cc, 5) == 0);
	CHECK(ltntstools_updatePacketWith64bCounter(pkt, sizeof(pkt), 0x40, &cc, 6) == 0);

	uint64_t current = 0;
	CHECK(ltntstools_verifyPacketWith64bCounter(pkt, sizeof(pkt), 0x40, 5, &current) == 0);
	CHECK(current == 6);
}

/* -------- pts_to_ascii / pcr_to_ascii -------- */

static void test_pts_to_ascii_known_values(void)
{
	char buf[32];
	char *p = buf;

	ltntstools_pts_to_ascii(&p, 0);
	CHECK(strcmp(buf, "0.00:00:00.000") == 0);

	p = buf;
	ltntstools_pts_to_ascii(&p, 90000LL * 3661); /* 1hr 1min 1sec */
	CHECK(strcmp(buf, "0.01:01:01.000") == 0);

	p = buf;
	ltntstools_pts_to_ascii(&p, 941040); /* 10.456 seconds */
	CHECK(strcmp(buf, "0.00:00:10.456") == 0);
}

static void test_pts_to_ascii_autoalloc(void)
{
	char *buf = NULL;
	ltntstools_pts_to_ascii(&buf, 0);
	CHECK(buf != NULL);
	if (buf) {
		CHECK(strcmp(buf, "0.00:00:00.000") == 0);
		free(buf);
	}
}

static void test_pcr_to_ascii_matches_pts_to_ascii(void)
{
	char pts_buf[32];
	char *p = pts_buf;
	int64_t pts = 90000LL * 3661;
	ltntstools_pts_to_ascii(&p, pts);

	char pcr_buf[32];
	char *q = pcr_buf;
	ltntstools_pcr_to_ascii(&q, pts * 300); /* pcr/300 == pts exactly */

	CHECK(strcmp(pts_buf, pcr_buf) == 0);
}

/* Regression test: the docs promise "pass a buffer of at least 16 bytes".
 * INT64_MAX/INT64_MIN produce a day count (plus, for negatives, extra '-'
 * signs on every field) that needs far more than 16 bytes of formatted text.
 * The old sprintf() would write straight past a 16-byte buffer; the fix
 * (snprintf(*buf, 16, ...)) must truncate instead. Buffer is mmap-guarded
 * so any write past byte 16 faults immediately rather than silently
 * corrupting adjacent memory. */
static void test_pts_to_ascii_extreme_positive_value_stays_in_bounds(void)
{
	struct guarded_buf g;
	CHECK(guarded_buf_alloc(&g, 16) == 0);

	char *p = (char *)g.ptr;
	ltntstools_pts_to_ascii(&p, INT64_MAX);

	CHECK(p == (char *)g.ptr); /* caller-supplied buffer must not be replaced */
	CHECK(memchr(g.ptr, '\0', 16) != NULL); /* nul terminator lands within bounds */
	CHECK(strlen((char *)g.ptr) < 16);

	guarded_buf_free(&g);
}

static void test_pts_to_ascii_extreme_negative_value_stays_in_bounds(void)
{
	struct guarded_buf g;
	CHECK(guarded_buf_alloc(&g, 16) == 0);

	char *p = (char *)g.ptr;
	ltntstools_pts_to_ascii(&p, INT64_MIN);

	CHECK(p == (char *)g.ptr);
	CHECK(memchr(g.ptr, '\0', 16) != NULL);
	CHECK(strlen((char *)g.ptr) < 16);

	guarded_buf_free(&g);
}

static void test_pcr_to_ascii_extreme_value_stays_in_bounds(void)
{
	struct guarded_buf g;
	CHECK(guarded_buf_alloc(&g, 16) == 0);

	char *p = (char *)g.ptr;
	ltntstools_pcr_to_ascii(&p, INT64_MAX);

	CHECK(memchr(g.ptr, '\0', 16) != NULL);
	CHECK(strlen((char *)g.ptr) < 16);

	guarded_buf_free(&g);
}

/* Auto-allocation path (buf == NULL) must also stay within the 16 bytes it
 * mallocs, even for a pts that needs far more digits than fit. */
static void test_pts_to_ascii_autoalloc_extreme_value_stays_in_bounds(void)
{
	char *buf = NULL;
	ltntstools_pts_to_ascii(&buf, INT64_MAX);
	CHECK(buf != NULL);
	if (buf) {
		CHECK(strlen(buf) < 16);
		free(buf);
	}
}

int main(void)
{
	test_sync_present();
	test_tei_set();
	test_pusi();
	test_transport_priority();
	test_pid();
	test_transport_scrambling_control();
	test_adaption_field_control();
	test_has_adaption();
	test_adaption_field_length();
	test_continuity_counter();

	test_pcrToScr_known_values();
	test_pcr_packTo_roundtrip();
	test_scr_rejects_no_sync();
	test_scr_rejects_no_adaptation();
	test_scr_rejects_adaptation_length_zero();
	test_scr_rejects_pcr_flag_not_set();
	test_scr_extracts_valid_pcr();

	test_scr_diff_no_wrap();
	test_scr_diff_wrap();
	test_scr_add_no_wrap();
	test_scr_add_overflow_wraps();
	test_scr_add_negative_wraps();
	test_pts_diff_no_wrap();
	test_pts_diff_wrap();

	test_contains_pes_header_found();
	test_contains_pes_header_not_found();
	test_contains_pes_header_forward_vs_reverse();
	test_contains_pes_header_short_buffer_safe();

	test_get_section_tableid_no_adaptation();
	test_get_section_tableid_with_adaptation();

	test_is_ESPayloadType_Video_spotcheck();
	test_is_ESPayloadType_Audio_spotcheck();
	test_GetESPayloadTypeDescription_never_null();

	test_generateNullPacket();
	test_findSyncPosition_aligned();
	test_findSyncPosition_offset();
	test_findSyncPosition_too_short();
	test_findSyncPosition_none_found();

	test_queryPCRs_finds_all_pcr_bearing_packets();
	test_queryPCR_pid_aligned_finds_match();
	test_queryPCR_pid_unaligned_offset();
	test_queryPCR_pid_no_match_returns_error();
	test_pcr_position_append_grows_array();
	test_queryPCR_pid_tiny_aligned_buffer_no_oob_read();
	test_queryPCRs_trailing_partial_packet_no_oob_read();
	test_queryPCR_pid_trailing_partial_packet_no_oob_read();

	test_generate_verify_roundtrip();
	test_verify_detects_wrong_pid();
	test_verify_detects_counter_mismatch();
	test_verify_detects_payload_corruption();
	test_update_then_verify();

	test_pts_to_ascii_known_values();
	test_pts_to_ascii_autoalloc();
	test_pcr_to_ascii_matches_pts_to_ascii();
	test_pts_to_ascii_extreme_positive_value_stays_in_bounds();
	test_pts_to_ascii_extreme_negative_value_stays_in_bounds();
	test_pcr_to_ascii_extreme_value_stays_in_bounds();
	test_pts_to_ascii_autoalloc_extreme_value_stays_in_bounds();

	if (g_failures == 0) {
		printf("PASS: all ts tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d ts test(s) failed\n", g_failures);
	return 1;
}
