/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/sectionextractor.c / src/libltntstools/sectionextractor.h.
 * Builds against ../src/sectionextractor.c plus ../src/ts.c and ../src/crc32.c.
 *
 * Two real bugs were found while writing these tests (confirmed with
 * standalone repros) and have since been fixed in src/sectionextractor.c:
 *
 * 1. Cross-call data loss: ltntstools_sectionextractor_write_packet()'s
 *    fallback "else" branch used to unconditionally clear ctx->complete for
 *    any packet that didn't start or continue a section -- including when
 *    ctx->complete was already 1 from a previous, still-unqueried
 *    completion. Calling ltntstools_sectionextractor_write() again before
 *    querying a just-completed section would silently discard it. Fixed by
 *    leaving ctx->complete (and the section bytes) untouched whenever a
 *    completed section is already pending.
 *
 * 2. ltntstools_sectionextractor_query() used to copy and report
 *    ctx->sectionLength + 3 bytes, but ctx->sectionLength already IS the
 *    full physical section size (table_id + 2 length bytes + payload +
 *    CRC). The extra +3 copied 3 bytes past the end of the real section,
 *    exposing uninitialized/stale bytes from the 4096-byte internal
 *    buffer. Fixed to use ctx->sectionLength directly.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/sectionextractor.h"
#include "libltntstools/ts.h"
#include "libltntstools/crc32.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* Builds a single TS packet containing a complete, standalone section:
 * PUSI=1, pointer_field=0, given table_id, and a valid trailing CRC32.
 * payloadLen must be small enough that table_id+2 len bytes+payload+CRC
 * fits in one packet (<= 183 bytes after the 5-byte TS+pointer header). */
static void build_section_packet(uint8_t *pkt, uint16_t pid, uint8_t tableId, uint8_t cc,
	const uint8_t *payload, int payloadLen)
{
	memset(pkt, 0xFF, 188);
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((pid >> 8) & 0x1f); /* PUSI=1 */
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10 | (cc & 0x0f); /* payload only, no adaptation */
	pkt[4] = 0x00; /* pointer_field */

	int seclen_field = payloadLen + 4; /* payload + 4-byte CRC */
	pkt[5] = tableId;
	pkt[6] = 0xB0 | ((seclen_field >> 8) & 0x0f);
	pkt[7] = seclen_field & 0xff;
	memcpy(&pkt[8], payload, payloadLen);

	uint32_t crc;
	ltntstools_getCRC32(&pkt[5], 3 + payloadLen, &crc);
	pkt[8 + payloadLen + 0] = (crc >> 24) & 0xff;
	pkt[8 + payloadLen + 1] = (crc >> 16) & 0xff;
	pkt[8 + payloadLen + 2] = (crc >> 8) & 0xff;
	pkt[8 + payloadLen + 3] = crc & 0xff;
}

static int section_total_len(int payloadLen)
{
	return 3 /* table_id + 2 length bytes */ + payloadLen + 4 /* CRC */;
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_sectionextractor_alloc(&hdl, 0x100, 0xFC) == 0);
	CHECK(hdl != NULL);
	ltntstools_sectionextractor_free(hdl);
}

static void test_free_null_safe(void)
{
	ltntstools_sectionextractor_free(NULL);
	CHECK(1);
}

/* -------- happy path: single-packet section -------- */

static void test_single_packet_section_completes_with_valid_crc(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payload[6] = { 'A', 'A', 'A', 'A', 'A', 'A' };

	uint8_t pkt[188];
	build_section_packet(pkt, pid, tableId, 0, payload, sizeof(payload));

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ssize_t ret = ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);

	CHECK(ret == section_total_len(sizeof(payload)));
	CHECK(complete == 1);
	CHECK(crcValid == 1);

	ltntstools_sectionextractor_free(hdl);
}

static void test_query_before_complete_fails(void)
{
	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, 0x100, 0xFC);

	uint8_t out[64];
	CHECK(ltntstools_sectionextractor_query(hdl, out, sizeof(out)) < 0);

	ltntstools_sectionextractor_free(hdl);
}

static void test_query_content_and_reset(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payload[6] = { 'A', 'B', 'C', 'D', 'E', 'F' };

	uint8_t pkt[188];
	build_section_packet(pkt, pid, tableId, 0, payload, sizeof(payload));

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);
	CHECK(complete == 1);

	uint8_t out[section_total_len(sizeof(payload)) + 16];
	int qret = ltntstools_sectionextractor_query(hdl, out, sizeof(out));
	CHECK(qret > 0);
	if (qret > 0) {
		CHECK(out[0] == tableId);
		CHECK(memcmp(&out[3], payload, sizeof(payload)) == 0);
	}

	/* Querying again without a new completed section must fail. */
	CHECK(ltntstools_sectionextractor_query(hdl, out, sizeof(out)) < 0);

	ltntstools_sectionextractor_free(hdl);
}

/* query() should copy/report exactly ctx->sectionLength bytes -- the real,
 * full, CRC-verified section size. */
static void test_query_length_matches_actual_section(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payload[6] = { 'A', 'A', 'A', 'A', 'A', 'A' };

	uint8_t pkt[188];
	build_section_packet(pkt, pid, tableId, 0, payload, sizeof(payload));

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);

	uint8_t out[64];
	int qret = ltntstools_sectionextractor_query(hdl, out, sizeof(out));
	CHECK(qret == section_total_len(sizeof(payload)));

	ltntstools_sectionextractor_free(hdl);
}

/* -------- filtering -------- */

static void test_wrong_pid_ignored(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payload[4] = { 1, 2, 3, 4 };

	uint8_t pkt[188];
	build_section_packet(pkt, 0x200 /* different pid */, tableId, 0, payload, sizeof(payload));

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ssize_t ret = ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);
	CHECK(ret == 0);
	CHECK(complete == 0);

	ltntstools_sectionextractor_free(hdl);
}

static void test_wrong_table_id_ignored(void)
{
	uint16_t pid = 0x100;
	uint8_t payload[4] = { 1, 2, 3, 4 };

	uint8_t pkt[188];
	build_section_packet(pkt, pid, 0xEE /* different table */, 0, payload, sizeof(payload));

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, 0xFC);

	int complete = 0, crcValid = 0;
	ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);
	CHECK(complete == 0);

	uint8_t out[64];
	CHECK(ltntstools_sectionextractor_query(hdl, out, sizeof(out)) < 0);

	ltntstools_sectionextractor_free(hdl);
}

static void test_bad_sync_byte_ignored(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payload[4] = { 1, 2, 3, 4 };

	uint8_t pkt[188];
	build_section_packet(pkt, pid, tableId, 0, payload, sizeof(payload));
	pkt[0] = 0x46; /* corrupt sync byte */

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ssize_t ret = ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);
	CHECK(ret == 0);
	CHECK(complete == 0);

	ltntstools_sectionextractor_free(hdl);
}

/* -------- CRC validation -------- */

static void test_corrupted_payload_completes_but_crc_invalid(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payload[6] = { 'A', 'A', 'A', 'A', 'A', 'A' };

	uint8_t pkt[188];
	build_section_packet(pkt, pid, tableId, 0, payload, sizeof(payload));
	pkt[9] ^= 0xFF; /* flip a payload byte after CRC was computed */

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 1;
	ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);
	CHECK(complete == 1); /* length still matches, so it's still "complete" */
	CHECK(crcValid == 0);

	ltntstools_sectionextractor_free(hdl);
}

/* -------- adaptation field handling -------- */

static void test_packet_with_adaptation_field_offset(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payload[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

	uint8_t pkt[188];
	memset(pkt, 0xFF, 188);
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((pid >> 8) & 0x1f); /* PUSI=1 */
	pkt[2] = pid & 0xff;
	pkt[3] = 0x30; /* adaptation + payload, cc=0 */
	pkt[4] = 3;    /* adaptation_field_length */
	pkt[5] = 0x00; pkt[6] = 0x00; pkt[7] = 0x00; /* adaptation stuffing */
	pkt[8] = 0x00; /* pointer_field, right after adaptation */

	int seclen_field = sizeof(payload) + 4;
	pkt[9] = tableId;
	pkt[10] = 0xB0 | ((seclen_field >> 8) & 0x0f);
	pkt[11] = seclen_field & 0xff;
	memcpy(&pkt[12], payload, sizeof(payload));

	uint32_t crc;
	ltntstools_getCRC32(&pkt[9], 3 + sizeof(payload), &crc);
	pkt[12 + sizeof(payload) + 0] = (crc >> 24) & 0xff;
	pkt[12 + sizeof(payload) + 1] = (crc >> 16) & 0xff;
	pkt[12 + sizeof(payload) + 2] = (crc >> 8) & 0xff;
	pkt[12 + sizeof(payload) + 3] = crc & 0xff;

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);
	CHECK(complete == 1);
	CHECK(crcValid == 1);

	ltntstools_sectionextractor_free(hdl);
}

/* -------- multi-packet section spanning -------- */

static void test_section_spanning_two_packets(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;

	/* 200-byte payload forces the section (3 + 200 + 4 = 207 bytes) past
	 * the 183-byte single-packet cap. */
	int payloadLen = 200;
	uint8_t *payload = malloc(payloadLen);
	for (int i = 0; i < payloadLen; i++) {
		payload[i] = (uint8_t)i;
	}

	int seclen_field = payloadLen + 4;
	uint8_t *section = malloc(3 + payloadLen + 4);
	section[0] = tableId;
	section[1] = 0xB0 | ((seclen_field >> 8) & 0x0f);
	section[2] = seclen_field & 0xff;
	memcpy(&section[3], payload, payloadLen);
	uint32_t crc;
	ltntstools_getCRC32(section, 3 + payloadLen, &crc);
	section[3 + payloadLen + 0] = (crc >> 24) & 0xff;
	section[3 + payloadLen + 1] = (crc >> 16) & 0xff;
	section[3 + payloadLen + 2] = (crc >> 8) & 0xff;
	section[3 + payloadLen + 3] = crc & 0xff;
	int totalSectionBytes = 3 + payloadLen + 4;

	uint8_t pkt1[188], pkt2[188];
	memset(pkt1, 0xFF, 188);
	pkt1[0] = 0x47;
	pkt1[1] = 0x40 | ((pid >> 8) & 0x1f);
	pkt1[2] = pid & 0xff;
	pkt1[3] = 0x10;
	pkt1[4] = 0x00; /* pointer_field */
	int firstChunk = totalSectionBytes < 183 ? totalSectionBytes : 183;
	memcpy(&pkt1[5], section, firstChunk);

	memset(pkt2, 0xFF, 188);
	pkt2[0] = 0x47;
	pkt2[1] = (pid >> 8) & 0x1f; /* PUSI=0, this is a continuation */
	pkt2[2] = pid & 0xff;
	pkt2[3] = 0x11;
	int remaining = totalSectionBytes - firstChunk;
	memcpy(&pkt2[4], section + firstChunk, remaining);

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ltntstools_sectionextractor_write(hdl, pkt1, 1, &complete, &crcValid);
	CHECK(complete == 0); /* not enough yet */

	complete = 0;
	ltntstools_sectionextractor_write(hdl, pkt2, 1, &complete, &crcValid);
	CHECK(complete == 1);
	CHECK(crcValid == 1);

	uint8_t out[300];
	int qret = ltntstools_sectionextractor_query(hdl, out, sizeof(out));
	CHECK(qret > 0);
	if (qret > 0) {
		CHECK(out[0] == tableId);
		CHECK(memcmp(&out[3], payload, payloadLen) == 0);
	}

	free(payload);
	free(section);
	ltntstools_sectionextractor_free(hdl);
}

/* -------- correct usage pattern: query immediately after each completion -------- */

static void test_multiple_sections_with_query_between_each(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	const char *payloads[3] = { "AAAAAA", "BBBBBB", "CCCCCC" };
	for (int n = 0; n < 3; n++) {
		uint8_t pkt[188];
		build_section_packet(pkt, pid, tableId, (uint8_t)n, (const uint8_t *)payloads[n], 6);

		int complete = 0, crcValid = 0;
		ltntstools_sectionextractor_write(hdl, pkt, 1, &complete, &crcValid);
		CHECK(complete == 1);
		CHECK(crcValid == 1);

		uint8_t out[64];
		int qret = ltntstools_sectionextractor_query(hdl, out, sizeof(out));
		CHECK(qret > 0);
		if (qret > 0) {
			CHECK(memcmp(&out[3], payloads[n], 6) == 0);
		}
	}

	ltntstools_sectionextractor_free(hdl);
}

static void test_write_without_query_between_calls_loses_section(void)
{
	uint16_t pid = 0x100;
	uint8_t tableId = 0xFC;
	uint8_t payloadA[6] = { 'A', 'A', 'A', 'A', 'A', 'A' };
	uint8_t payloadB[6] = { 'B', 'B', 'B', 'B', 'B', 'B' };

	uint8_t pktA[188], pktB[188];
	build_section_packet(pktA, pid, tableId, 0, payloadA, sizeof(payloadA));
	build_section_packet(pktB, pid, tableId, 1, payloadB, sizeof(payloadB));

	void *hdl;
	ltntstools_sectionextractor_alloc(&hdl, pid, tableId);

	int complete = 0, crcValid = 0;
	ltntstools_sectionextractor_write(hdl, pktA, 1, &complete, &crcValid);
	CHECK(complete == 1); /* section A is ready */

	/* Caller doesn't query yet -- a second _write() call (e.g. the next
	 * batch of incoming TS packets) arrives first. Intended behaviour:
	 * section A should still be queryable afterward, since it was never
	 * retrieved. */
	complete = 0;
	ltntstools_sectionextractor_write(hdl, pktB, 1, &complete, &crcValid);

	uint8_t out[64];
	int qret = ltntstools_sectionextractor_query(hdl, out, sizeof(out));
	CHECK(qret > 0);
	if (qret > 0) {
		CHECK(memcmp(&out[3], payloadA, sizeof(payloadA)) == 0);
	}

	ltntstools_sectionextractor_free(hdl);
}

int main(void)
{
	test_alloc_free_basic();
	test_free_null_safe();

	test_single_packet_section_completes_with_valid_crc();
	test_query_before_complete_fails();
	test_query_content_and_reset();
	test_query_length_matches_actual_section();

	test_wrong_pid_ignored();
	test_wrong_table_id_ignored();
	test_bad_sync_byte_ignored();

	test_corrupted_payload_completes_but_crc_invalid();

	test_packet_with_adaptation_field_offset();

	test_section_spanning_two_packets();

	test_multiple_sections_with_query_between_each();
	test_write_without_query_between_calls_loses_section();

	if (g_failures == 0) {
		printf("PASS: all sectionextractor tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d sectionextractor test(s) failed\n", g_failures);
	return 1;
}
