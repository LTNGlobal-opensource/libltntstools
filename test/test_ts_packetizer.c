/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/ts_packetizer.c / src/libltntstools/ts_packetizer.h.
 * Builds against ../src/ts_packetizer.c plus ../src/ts.c (only used here for
 * its header-only PID/adaptation-field accessors and ltntstools_scr(), to
 * cross-check PCR encoding against already-tested infrastructure).
 *
 * TWO REAL BUGS were found in ltntstools_ts_packetizer() (the plain,
 * no-PCR variant) while writing these tests, confirmed with a standalone
 * repro, and have been FIXED in src/ts_packetizer.c as part of this change:
 *
 * 1. Oversized allocation: `packets = ((byteCount / max) + 1) * packetSize;`
 *    computed a *byte* count (already multiplied by packetSize once), then
 *    `calloc(packets, packetSize)` multiplied by packetSize AGAIN, over
 *    allocating by a factor of ~packetSize (188x). Confirmed via
 *    malloc_size(): a 1000-byte input allocated ~213KB instead of ~1.3KB.
 *    Fixed to `packets = (byteCount / max) + 1;` (a real packet count).
 *
 * 2. Non-compliant trailing padding: the last (short) packet was always
 *    written with adaptation_field_control = payload-only (0x10), with any
 *    leftover space filled by raw 0xFF bytes appended directly to the
 *    payload region via memset(). A spec-compliant decoder has no way to
 *    distinguish those trailing 0xFF bytes from real elementary-stream
 *    data -- payload-only packets declare their *entire* post-header
 *    region as payload. Fixed to use a proper adaptation field
 *    (adaptation_field_control = 0x30) with a correct adaptation_field_length
 *    to consume the unused space, mirroring the (already-correct) pattern
 *    already used by the sibling ltntstools_ts_packetizer_with_pcr().
 *
 * ltntstools_ts_packetizer_with_pcr() was carefully re-derived by hand
 * (adaptation_field_length arithmetic across all three of its branches --
 * PCR, RAI/ESPI-only, plain) and found to already be correct; no bugs were
 * found there.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

#include "libltntstools/ts_packetizer.h"
#include "libltntstools/ts.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* Locate the payload region of a single 188-byte TS packet, accounting for
 * an optional adaptation field. Uses ts.c's own (already-tested) accessors. */
static const uint8_t *payload_ptr(const uint8_t *pkt, int *lenOut)
{
	int offset = 4;
	if (ltntstools_has_adaption((uint8_t *)pkt)) {
		offset += 1 + ltntstools_adaption_field_length((uint8_t *)pkt);
	}
	*lenOut = 188 - offset;
	return pkt + offset;
}

/* -------- ltntstools_ts_packetizer() argument validation -------- */

static void test_rejects_invalid_args(void)
{
	uint8_t buf[10] = { 0 };
	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;

	CHECK(ltntstools_ts_packetizer(NULL, sizeof(buf), &pkts, &count, 188, &cc, 0x100) < 0);
	CHECK(ltntstools_ts_packetizer(buf, 0, &pkts, &count, 188, &cc, 0x100) < 0);
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), NULL, &count, 188, &cc, 0x100) < 0);
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, NULL, 188, &cc, 0x100) < 0);
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 187, &cc, 0x100) < 0);
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, NULL, 0x100) < 0);
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x2000) < 0);
}

/* -------- ltntstools_ts_packetizer(): exact-fit packets -------- */

static void test_single_packet_exact_fit(void)
{
	uint8_t buf[184];
	for (int i = 0; i < 184; i++) {
		buf[i] = (uint8_t)i;
	}

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100) == 0);
	CHECK(count == 1);

	CHECK(pkts[0] == 0x47);
	CHECK(ltntstools_pid(pkts) == 0x100);
	CHECK(ltntstools_payload_unit_start_indicator(pkts) == 1);
	CHECK(ltntstools_has_adaption(pkts) == 0);
	CHECK(ltntstools_adaption_field_control(pkts) == 1); /* payload only */

	int len = 0;
	const uint8_t *p = payload_ptr(pkts, &len);
	CHECK(len == 184);
	CHECK(memcmp(p, buf, 184) == 0);

	free(pkts);
}

static void test_multi_packet_exact_multiple(void)
{
	uint8_t buf[184 * 3];
	for (int i = 0; i < (int)sizeof(buf); i++) {
		buf[i] = (uint8_t)(i & 0xff);
	}

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x200) == 0);
	CHECK(count == 3);

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *pkt = pkts + i * 188;
		CHECK(pkt[0] == 0x47);
		CHECK(ltntstools_has_adaption(pkt) == 0);
		CHECK(ltntstools_payload_unit_start_indicator(pkt) == (i == 0 ? 1 : 0));

		int len = 0;
		const uint8_t *p = payload_ptr(pkt, &len);
		CHECK(len == 184);
		CHECK(memcmp(p, buf + i * 184, 184) == 0);
	}

	free(pkts);
}

/* -------- ltntstools_ts_packetizer(): short/remainder packet -------- */

static void test_short_final_packet_uses_adaptation_field(void)
{
	uint8_t buf[80];
	for (int i = 0; i < 80; i++) {
		buf[i] = (uint8_t)(0x40 + i);
	}

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x300) == 0);
	CHECK(count == 1);

	const uint8_t *pkt = pkts;
	CHECK(ltntstools_has_adaption(pkt) == 1);
	CHECK(ltntstools_adaption_field_control(pkt) == 3); /* adaptation + payload */

	int expectedAdaptLen = (188 - 4 - 1) - 80; /* 183 - 80 = 103 */
	CHECK(ltntstools_adaption_field_length(pkt) == expectedAdaptLen);

	int len = 0;
	const uint8_t *p = payload_ptr(pkt, &len);
	CHECK(len == 80);
	CHECK(memcmp(p, buf, 80) == 0);

	free(pkts);
}

static void test_multi_packet_with_remainder(void)
{
	uint8_t buf[1000];
	for (int i = 0; i < (int)sizeof(buf); i++) {
		buf[i] = (uint8_t)(i & 0xff);
	}

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100) == 0);

	/* 1000 / 184 = 5 full packets (920 bytes) + 1 short packet (80 bytes). */
	CHECK(count == 6);

	uint8_t *reassembled = malloc(sizeof(buf));
	int reassembledLen = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *pkt = pkts + i * 188;
		CHECK(pkt[0] == 0x47);
		int len = 0;
		const uint8_t *p = payload_ptr(pkt, &len);
		memcpy(reassembled + reassembledLen, p, len);
		reassembledLen += len;
	}
	CHECK(reassembledLen == (int)sizeof(buf));
	CHECK(memcmp(reassembled, buf, sizeof(buf)) == 0);

	/* Last packet specifically must use an adaptation field, not raw
	 * payload padding (this is the bug that was fixed). */
	const uint8_t *last = pkts + (count - 1) * 188;
	CHECK(ltntstools_has_adaption(last) == 1);

	free(reassembled);
	free(pkts);
}

static void test_allocation_size_is_reasonable(void)
{
#ifdef __APPLE__
	uint8_t buf[1000];
	memset(buf, 0xAB, sizeof(buf));

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100);

	size_t needed = (size_t)count * 188;
	size_t actual = malloc_size(pkts);
	/* Allow normal malloc size-class rounding, but not the ~188x
	 * over-allocation the unfixed code used to produce. */
	CHECK(actual < needed * 2);

	free(pkts);
#else
	CHECK(1); /* malloc_size() is macOS-specific; skip elsewhere. */
#endif
}

static void test_cc_increments_and_wraps(void)
{
	uint8_t buf[184 * 20];
	memset(buf, 0x11, sizeof(buf));

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 14; /* seed near the 4-bit wrap boundary */
	CHECK(ltntstools_ts_packetizer(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100) == 0);
	CHECK(count == 20);

	for (uint32_t i = 0; i < count; i++) {
		uint8_t expected = (uint8_t)((14 + i) & 0x0f);
		CHECK(ltntstools_continuity_counter(pkts + i * 188) == expected);
	}
	CHECK(cc == (uint8_t)((14 + 20) & 0xff)); /* cc storage itself isn't masked, only the on-wire nibble is */

	free(pkts);
}

/* -------- ltntstools_ts_packetizer_with_pcr() -------- */

static void test_pcr_rejects_invalid_args(void)
{
	uint8_t buf[10] = { 0 };
	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;

	CHECK(ltntstools_ts_packetizer_with_pcr(NULL, sizeof(buf), &pkts, &count, 188, &cc, 0x100, -1, 0, 0) < 0);
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, 0, &pkts, &count, 188, &cc, 0x100, -1, 0, 0) < 0);
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 187, &cc, 0x100, -1, 0, 0) < 0);
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x2000, -1, 0, 0) < 0);
}

static void test_pcr_no_pcr_exact_fit(void)
{
	uint8_t buf[184];
	for (int i = 0; i < 184; i++) buf[i] = (uint8_t)i;

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100, -1, 0, 0) == 0);
	CHECK(count == 1);
	CHECK(ltntstools_has_adaption(pkts) == 0);
	CHECK(ltntstools_payload_unit_start_indicator(pkts) == 1);

	int len = 0;
	const uint8_t *p = payload_ptr(pkts, &len);
	CHECK(len == 184);
	CHECK(memcmp(p, buf, 184) == 0);

	free(pkts);
}

static void test_pcr_short_final_packet_no_pcr(void)
{
	uint8_t buf[50];
	memset(buf, 0x77, sizeof(buf));

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100, -1, 0, 0) == 0);
	CHECK(count == 1);
	CHECK(ltntstools_has_adaption(pkts) == 1);

	int len = 0;
	const uint8_t *p = payload_ptr(pkts, &len);
	CHECK(len == (int)sizeof(buf));
	CHECK(memcmp(p, buf, sizeof(buf)) == 0);

	free(pkts);
}

static void test_pcr_inserted_and_decodes_correctly(void)
{
	uint8_t buf[300]; /* forces 2 packets: rem=300 on first iter >=176, so cpy=176 first packet */
	for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (uint8_t)(i & 0xff);

	int64_t pcrIn = 123456789LL;

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100, pcrIn, 0, 0) == 0);
	CHECK(count >= 1);

	CHECK(ltntstools_has_adaption(pkts) == 1);
	CHECK(ltntstools_adaption_field_control(pkts) == 3);

	uint64_t scr = 0;
	CHECK(ltntstools_scr(pkts, &scr) == 0);
	CHECK((int64_t)scr == pcrIn);

	/* PCR must only be on the first packet. */
	for (uint32_t i = 1; i < count; i++) {
		uint64_t scr2;
		CHECK(ltntstools_scr(pkts + i * 188, &scr2) < 0);
	}

	free(pkts);
}

static void test_pcr_random_access_indicator_flag(void)
{
	uint8_t buf[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100, -1, 1 /* RAI */, 0) == 0);

	CHECK(ltntstools_has_adaption(pkts) == 1);
	/* adaptation flags byte is right after the length byte */
	uint8_t adaptationFlags = pkts[5];
	CHECK((adaptationFlags & 0x40) != 0); /* random_access_indicator bit */
	CHECK((adaptationFlags & 0x20) == 0); /* ES priority not requested */

	free(pkts);
}

static void test_pcr_elementary_stream_priority_indicator_flag(void)
{
	uint8_t buf[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100, -1, 0, 1 /* ESPI */) == 0);

	CHECK(ltntstools_has_adaption(pkts) == 1);
	uint8_t adaptationFlags = pkts[5];
	CHECK((adaptationFlags & 0x20) != 0);
	CHECK((adaptationFlags & 0x40) == 0);

	free(pkts);
}

static void test_pcr_combined_with_indicators(void)
{
	uint8_t buf[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	int64_t pcrIn = 999999;

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, sizeof(buf), &pkts, &count, 188, &cc, 0x100, pcrIn, 1, 1) == 0);

	uint64_t scr = 0;
	CHECK(ltntstools_scr(pkts, &scr) == 0);
	CHECK((int64_t)scr == pcrIn);

	uint8_t adaptationFlags = pkts[5];
	CHECK((adaptationFlags & 0x10) != 0); /* PCR_flag */
	CHECK((adaptationFlags & 0x40) != 0); /* RAI */
	CHECK((adaptationFlags & 0x20) != 0); /* ESPI */

	free(pkts);
}

static void test_pcr_large_buffer_content_integrity(void)
{
	int bufLen = 5000;
	uint8_t *buf = malloc(bufLen);
	for (int i = 0; i < bufLen; i++) buf[i] = (uint8_t)(i & 0xff);

	uint8_t *pkts = NULL;
	uint32_t count = 0;
	uint8_t cc = 0;
	CHECK(ltntstools_ts_packetizer_with_pcr(buf, bufLen, &pkts, &count, 188, &cc, 0x100, 42, 1, 0) == 0);

	uint8_t *reassembled = malloc(bufLen);
	int reassembledLen = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *pkt = pkts + i * 188;
		CHECK(pkt[0] == 0x47);
		CHECK(ltntstools_pid(pkt) == 0x100);
		int len = 0;
		const uint8_t *p = payload_ptr(pkt, &len);
		memcpy(reassembled + reassembledLen, p, len);
		reassembledLen += len;
	}
	CHECK(reassembledLen == bufLen);
	CHECK(memcmp(reassembled, buf, bufLen) == 0);

	free(buf);
	free(reassembled);
	free(pkts);
}

int main(void)
{
	test_rejects_invalid_args();
	test_single_packet_exact_fit();
	test_multi_packet_exact_multiple();
	test_short_final_packet_uses_adaptation_field();
	test_multi_packet_with_remainder();
	test_allocation_size_is_reasonable();
	test_cc_increments_and_wraps();

	test_pcr_rejects_invalid_args();
	test_pcr_no_pcr_exact_fit();
	test_pcr_short_final_packet_no_pcr();
	test_pcr_inserted_and_decodes_correctly();
	test_pcr_random_access_indicator_flag();
	test_pcr_elementary_stream_priority_indicator_flag();
	test_pcr_combined_with_indicators();
	test_pcr_large_buffer_content_integrity();

	if (g_failures == 0) {
		printf("PASS: all ts_packetizer tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d ts_packetizer test(s) failed\n", g_failures);
	return 1;
}
