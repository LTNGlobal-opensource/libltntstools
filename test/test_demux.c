/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/demux.c / src/demux-pid.c / src/libltntstools/demux.h.
 * Builds against ../src/demux.c ../src/demux-pid.c plus their real (non-
 * mocked) dependencies: ../src/pat.c (PAT/PMT model + libdvbpsi), ../src/
 * descriptor.c ../src/crc32.c ../src/ts.c, ../src/streammodel.c ../src/
 * streammodel-extractors.c ../src/sectionextractor.c (streammodel.c's own
 * deps), ../src/stats.c ../src/clocks.c ../src/history-metric.c (stats.c's
 * deps, also used directly by pes-extractor.c), and ../src/pes-extractor.c
 * ../src/klringbuffer.c ../src/utils.c ../src/pes.c (the PES extraction
 * pipeline demux.c drives per elementary stream). Links -lpthread and
 * libdvbpsi.
 *
 * SCOPE NOTE: src/libltntstools/demux.h itself is documented
 * "Experimental. Work in progress. Do not use." This file tests it as it
 * exists today; treat any test here as pinning current behavior, not as a
 * promise the shape won't change.
 *
 * Test PATs are built by hand (ltntstools_pat_alloc() + filling in fields
 * directly) rather than round-tripped through real dvbpsi TS parsing --
 * ltntstools_demux_alloc_from_pat() takes a struct ltntstools_pat_s
 * directly and clones it (per its own doc comment), so this is a faithful,
 * much simpler fixture than synthesizing and parsing real PAT/PMT sections.
 *
 * Real PES payloads are built the same way test_pes_extractor.c does:
 * pack a struct ltn_pes_packet_s to bytes, split into 188-byte TS packets
 * for the target pid, then append one throwaway PUSI=1 trailer packet --
 * ltntstools_pes_extractor_write() (which demux_write() drives per pid)
 * only flushes a PES to its callback when the *next* PUSI=1 packet for the
 * same pid arrives.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "libltntstools/pes.h"
#include "libltntstools/demux.h"
#include "libltntstools/pat.h"
#include "libltntstools/klbitstream_readwriter.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- building a real, valid, parseable PES byte buffer --------
 * Same technique as test_pes_extractor.c / test_pes.c's
 * pack_with_correct_length(): pack() writes whatever PES_packet_length is
 * already in the struct rather than computing it, so pack once, then patch
 * the length field bytes to match pack()'s own returned byte count.
 */
static int pack_pes(uint8_t streamId, const uint8_t *payload, int payloadLen, uint8_t *buf, int bufLen)
{
	struct ltn_pes_packet_s pkt = { 0 };
	pkt.packet_start_code_prefix = 0x000001;
	pkt.stream_id = streamId;
	pkt.data_alignment_indicator = 1;
	pkt.data = (unsigned char *)payload;
	pkt.dataLengthBytes = payloadLen;

	memset(buf, 0, bufLen);
	struct klbs_context_s bs;
	klbs_init(&bs);
	klbs_write_set_buffer(&bs, buf, bufLen);

	ssize_t bits = ltn_pes_packet_pack(&pkt, &bs);
	int totalBytes = (int)(bits / 8);
	uint16_t correctLength = (uint16_t)(totalBytes - 6);
	buf[4] = (correctLength >> 8) & 0xff;
	buf[5] = correctLength & 0xff;

	return totalBytes;
}

/* Splits `buf` into 188-byte TS packets for `pid`, PUSI=1 on the first
 * packet only, continuity counter threaded through *cc. Returns packets written. */
static int build_ts_packets(const uint8_t *buf, int bufLen, uint16_t pid, uint8_t *cc,
	uint8_t packets[][188], int maxPackets)
{
	int n = 0;
	int offset = 0;
	while (offset < bufLen && n < maxPackets) {
		uint8_t *pkt = packets[n];
		memset(pkt, 0xFF, 188);
		pkt[0] = 0x47;
		pkt[1] = ((offset == 0) ? 0x40 : 0x00) | ((pid >> 8) & 0x1f);
		pkt[2] = pid & 0xff;
		pkt[3] = 0x10 | (*cc & 0x0f);
		*cc = (*cc + 1) & 0x0f;

		int chunk = bufLen - offset;
		if (chunk > 184)
			chunk = 184;
		memcpy(&pkt[4], buf + offset, chunk);

		offset += chunk;
		n++;
	}
	return n;
}

static void build_trailer_packet(uint16_t pid, uint8_t *cc, uint8_t pkt[188])
{
	memset(pkt, 0xFF, 188);
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((pid >> 8) & 0x1f);
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10 | (*cc & 0x0f);
	*cc = (*cc + 1) & 0x0f;
	pkt[4] = 0x00;
}

/* -------- a minimal, hand-built PAT: one program, one video elementary stream -------- */

#define VIDEO_PID 0x100

static struct ltntstools_pat_s *build_single_video_pat(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	pat->transport_stream_id = 1;
	pat->current_next_indicator = 1;
	pat->program_count = 1;

	struct ltntstools_pat_program_s *prog = &pat->programs[0];
	prog->program_number = 1;
	prog->program_map_PID = 0x1000;

	prog->pmt.program_number = 1;
	prog->pmt.current_next_indicator = 1;
	prog->pmt.PCR_PID = VIDEO_PID;
	prog->pmt.stream_count = 1;
	prog->pmt.streams[0].stream_type = 0x1b; /* H.264 video */
	prog->pmt.streams[0].elementary_PID = VIDEO_PID;

	return pat;
}

/* -------- callback capture -------- */

struct captured_s {
	uint16_t pid;
	struct ltn_pes_packet_s *pes;
};

#define MAX_CAPTURED 16
static struct captured_s g_captured[MAX_CAPTURED];
static int g_capturedCount;

static void reset_capture(void)
{
	g_capturedCount = 0;
	memset(g_captured, 0, sizeof(g_captured));
}

/* Per demux.h's own doc comment: "Caller DOES NOT OWN the lifespan of the
 * returned object" -- demux.c keeps it on pid->pesList and frees it later
 * (on expiry or ltntstools_demux_free()), so this callback must NOT free it. */
static void capture_cb(void *userContext, uint16_t pid, struct ltn_pes_packet_s *pes)
{
	if (g_capturedCount < MAX_CAPTURED) {
		g_captured[g_capturedCount].pid = pid;
		g_captured[g_capturedCount].pes = pes;
		g_capturedCount++;
	}
}

/* ---------------------------------------------------------------------
 * alloc_from_pat() / free() -- lifecycle and argument validation
 * --------------------------------------------------------------------- */

static void test_alloc_from_pat_rejects_null_pat(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_demux_alloc_from_pat(&hdl, NULL, NULL, NULL) == -1);
}

static void test_alloc_and_free_empty_pat(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	pat->transport_stream_id = 1;
	pat->program_count = 0;

	void *hdl = NULL;
	CHECK(ltntstools_demux_alloc_from_pat(&hdl, NULL, NULL, pat) == 0);
	CHECK(hdl != NULL);

	ltntstools_demux_free(hdl);
	ltntstools_pat_free(pat);
}

static void test_alloc_clones_the_pat_caller_may_free_immediately(void)
{
	/* demux.h's doc comment: "This framework clones the PAT being passed,
	 * so you're free to end the PAT lifetime when the call completes." */
	struct ltntstools_pat_s *pat = build_single_video_pat();

	void *hdl = NULL;
	CHECK(ltntstools_demux_alloc_from_pat(&hdl, NULL, NULL, pat) == 0);
	ltntstools_pat_free(pat); /* freed immediately, per the documented contract */

	CHECK(hdl != NULL);
	ltntstools_demux_free(hdl);
}

/* ---------------------------------------------------------------------
 * write() -- argument validation
 * --------------------------------------------------------------------- */

static void test_write_rejects_invalid_args(void)
{
	struct ltntstools_pat_s *pat = build_single_video_pat();
	void *hdl = NULL;
	ltntstools_demux_alloc_from_pat(&hdl, NULL, NULL, pat);
	ltntstools_pat_free(pat);

	uint8_t pkt[188];
	memset(pkt, 0xFF, sizeof(pkt));

	CHECK(ltntstools_demux_write(NULL, pkt, 1) == -1);
	CHECK(ltntstools_demux_write(hdl, NULL, 1) == -1);
	CHECK(ltntstools_demux_write(hdl, pkt, 0) == -1);

	ltntstools_demux_free(hdl);
}

/* ---------------------------------------------------------------------
 * write() -- a real PES on the configured video pid reaches cb_pes
 * --------------------------------------------------------------------- */

static void test_video_pes_reaches_callback(void)
{
	struct ltntstools_pat_s *pat = build_single_video_pat();

	struct ltntstools_demux_callbacks callbacks = { .cb_pes = capture_cb };
	void *hdl = NULL;
	CHECK(ltntstools_demux_alloc_from_pat(&hdl, NULL, &callbacks, pat) == 0);
	ltntstools_pat_free(pat);
	reset_capture();

	uint8_t payload[16];
	for (int i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (uint8_t)(0xA0 + i);

	uint8_t pesBuf[64];
	int pesLen = pack_pes(0xE0, payload, sizeof(payload), pesBuf, sizeof(pesBuf));

	uint8_t packets[4][188];
	uint8_t cc = 0;
	int n = build_ts_packets(pesBuf, pesLen, VIDEO_PID, &cc, packets, 4);
	build_trailer_packet(VIDEO_PID, &cc, packets[n++]);

	CHECK(ltntstools_demux_write(hdl, &packets[0][0], n) == n);

	CHECK(g_capturedCount == 1);
	if (g_capturedCount == 1) {
		CHECK(g_captured[0].pid == VIDEO_PID);
		struct ltn_pes_packet_s *pes = g_captured[0].pes;
		CHECK(pes != NULL);
		if (pes) {
			CHECK(pes->stream_id == 0xE0);
			CHECK((int)pes->dataLengthBytes == (int)sizeof(payload));
			if ((int)pes->dataLengthBytes == (int)sizeof(payload)) {
				CHECK(memcmp(pes->data, payload, sizeof(payload)) == 0);
			}
		}
	}

	/* pes is still owned by demux (on pid->pesList) -- freed by demux_free(). */
	ltntstools_demux_free(hdl);
}

static void test_write_on_pid_not_in_pat_never_calls_back(void)
{
	struct ltntstools_pat_s *pat = build_single_video_pat();

	struct ltntstools_demux_callbacks callbacks = { .cb_pes = capture_cb };
	void *hdl = NULL;
	ltntstools_demux_alloc_from_pat(&hdl, NULL, &callbacks, pat);
	ltntstools_pat_free(pat);
	reset_capture();

	uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t pesBuf[64];
	int pesLen = pack_pes(0xE0, payload, sizeof(payload), pesBuf, sizeof(pesBuf));

	uint8_t packets[4][188];
	uint8_t cc = 0;
	/* 0x200 is not the configured video pid (0x100) and appears nowhere in the PAT/PMT. */
	int n = build_ts_packets(pesBuf, pesLen, 0x200, &cc, packets, 4);
	build_trailer_packet(0x200, &cc, packets[n++]);

	CHECK(ltntstools_demux_write(hdl, &packets[0][0], n) == n);
	CHECK(g_capturedCount == 0);

	ltntstools_demux_free(hdl);
}

int main(void)
{
	test_alloc_from_pat_rejects_null_pat();
	test_alloc_and_free_empty_pat();
	test_alloc_clones_the_pat_caller_may_free_immediately();
	test_write_rejects_invalid_args();
	test_video_pes_reaches_callback();
	test_write_on_pid_not_in_pat_never_calls_back();

	if (g_failures == 0) {
		printf("PASS: all demux tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d demux test(s) failed\n", g_failures);
	return 1;
}
