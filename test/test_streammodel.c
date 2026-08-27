/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/streammodel.c / src/libltntstools/streammodel.h.
 * Builds against ../src/streammodel.c plus ../src/streammodel-extractors.c
 * (referenced unconditionally from ltntstools_streammodel_free()),
 * ../src/pat.c, ../src/descriptor.c, ../src/crc32.c, ../src/ts.c,
 * ../src/sectionextractor.c (pulled in transitively by
 * streammodel-extractors.c), and ../src/stats.c plus its own
 * ../src/clocks.c/../src/history-metric.c dependencies (streammodel.c
 * calls ltntstools_isCCInError(), which lives in stats.c). Links against
 * libdvbpsi and -lpthread. See test/Makefile for how libdvbpsi is located.
 *
 * ltntstools_streammodel_is_model_mpts() and
 * ltntstools_streammodel_query_first_program_pcr_pid() don't touch the
 * `hdl` context argument at all -- they operate purely on the
 * caller-supplied struct ltntstools_pat_s -- so those are tested directly
 * against hand-built PAT objects, the same way test_pat.c builds them.
 *
 * ltntstools_streammodel_write()/_query_model() are the real integration
 * surface: they decode PAT/PMT tables from raw 188-byte transport packets
 * via libdvbpsi. Those are exercised end-to-end by encoding real PAT/PMT
 * packets with ltntstools_pat_create_packet_ts()/ltntstools_pmt_create_packet_ts()
 * (already unit-tested in test_pat.c to produce correct, CRC-valid
 * packets), feeding them through _write(), and checking the resulting
 * model via _query_model().
 *
 * Model-completion timing note: _rom_activate() (called once during
 * _alloc()) seeds the "next" model's allowableWriteTime at 500ms past
 * whatever ctx->now was at alloc time (0,0 on a freshly calloc'd context).
 * Passing a first write() timestamp at or beyond that (we use 1 second)
 * makes ctx->writePackets=1 on that very first call, so a PAT packet
 * followed immediately by its PMT packet(s) in the same write() buffer
 * completes the model synchronously within that single call -- no need to
 * simulate multiple calls or real wall-clock waiting.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "libltntstools/streammodel.h"
#include "libltntstools/pat.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

static void add_stream(struct ltntstools_pmt_s *pmt, uint32_t stream_type, uint32_t pid)
{
	struct ltntstools_pmt_entry_s *e = &pmt->streams[pmt->stream_count++];
	memset(e, 0, sizeof(*e));
	e->stream_type = stream_type;
	e->elementary_PID = pid;
}

static struct ltntstools_pat_program_s *add_program(struct ltntstools_pat_s *pat, uint32_t program_number, uint32_t pmt_pid)
{
	struct ltntstools_pat_program_s *pp = &pat->programs[pat->program_count++];
	memset(pp, 0, sizeof(*pp));
	pp->program_number = program_number;
	pp->program_map_PID = pmt_pid;
	pp->pmt.program_number = program_number;
	return pp;
}

/* -------- ltntstools_streammodel_is_model_mpts (pure, no hdl needed) -------- */

static void test_is_model_mpts_single_program_is_spts(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 1, 0x100);

	CHECK(ltntstools_streammodel_is_model_mpts(NULL, pat) == 0);

	ltntstools_pat_free(pat);
}

static void test_is_model_mpts_two_programs_is_mpts(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 1, 0x100);
	add_program(pat, 2, 0x200);

	CHECK(ltntstools_streammodel_is_model_mpts(NULL, pat) == 1);

	ltntstools_pat_free(pat);
}

static void test_is_model_mpts_ignores_network_pid_program_zero(void)
{
	/* program_number == 0 is the NIT/network entry, not a real service. */
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 0, 0x010);
	add_program(pat, 1, 0x100);

	CHECK(ltntstools_streammodel_is_model_mpts(NULL, pat) == 0);

	ltntstools_pat_free(pat);
}

/* -------- ltntstools_streammodel_query_first_program_pcr_pid (pure) -------- */

static void test_query_first_program_pcr_pid_null_args_fail(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 1, 0x100);
	uint16_t pcrpid = 0;

	CHECK(ltntstools_streammodel_query_first_program_pcr_pid(NULL, NULL, &pcrpid) == -1);
	CHECK(ltntstools_streammodel_query_first_program_pcr_pid(NULL, pat, NULL) == -1);

	ltntstools_pat_free(pat);
}

static void test_query_first_program_pcr_pid_returns_first_match(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	struct ltntstools_pat_program_s *pp = add_program(pat, 1, 0x100);
	add_stream(&pp->pmt, 0x1b /* AVC */, 0x101);
	pp->pmt.PCR_PID = 0x101;

	uint16_t pcrpid = 0;
	CHECK(ltntstools_streammodel_query_first_program_pcr_pid(NULL, pat, &pcrpid) == 0);
	CHECK(pcrpid == 0x101);

	ltntstools_pat_free(pat);
}

static void test_query_first_program_pcr_pid_skips_streamless_programs(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 1, 0x100); /* no streams -- must be skipped */
	struct ltntstools_pat_program_s *pp2 = add_program(pat, 2, 0x200);
	add_stream(&pp2->pmt, 0x0f /* AAC */, 0x201);
	pp2->pmt.PCR_PID = 0x201;

	uint16_t pcrpid = 0;
	CHECK(ltntstools_streammodel_query_first_program_pcr_pid(NULL, pat, &pcrpid) == 0);
	CHECK(pcrpid == 0x201);

	ltntstools_pat_free(pat);
}

static void test_query_first_program_pcr_pid_no_pcr_set_fails(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	struct ltntstools_pat_program_s *pp = add_program(pat, 1, 0x100);
	add_stream(&pp->pmt, 0x1b, 0x101);
	pp->pmt.PCR_PID = 0; /* never set */

	uint16_t pcrpid = 0xffff;
	CHECK(ltntstools_streammodel_query_first_program_pcr_pid(NULL, pat, &pcrpid) == -1);

	ltntstools_pat_free(pat);
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_streammodel_alloc(&hdl, NULL) == 0);
	CHECK(hdl != NULL);
	ltntstools_streammodel_free(hdl);
}

static void test_get_current_version_starts_at_zero(void)
{
	void *hdl = NULL;
	ltntstools_streammodel_alloc(&hdl, NULL);

	CHECK(ltntstools_streammodel_get_current_version(hdl) == 0);

	ltntstools_streammodel_free(hdl);
}

/* -------- ltntstools_streammodel_write argument validation -------- */

static void test_write_rejects_null_hdl(void)
{
	uint8_t pkt[188];
	memset(pkt, 0xff, sizeof(pkt));
	int complete = -1;
	struct timeval ts = { 1, 0 };

	size_t ret = ltntstools_streammodel_write(NULL, pkt, 1, &complete, &ts);
	CHECK(ret == (size_t)-1);
}

static void test_write_rejects_null_packet_buffer(void)
{
	void *hdl = NULL;
	ltntstools_streammodel_alloc(&hdl, NULL);

	int complete = -1;
	struct timeval ts = { 1, 0 };
	size_t ret = ltntstools_streammodel_write(hdl, NULL, 1, &complete, &ts);
	CHECK(ret == (size_t)-1);

	ltntstools_streammodel_free(hdl);
}

static void test_write_rejects_non_positive_packet_count(void)
{
	void *hdl = NULL;
	ltntstools_streammodel_alloc(&hdl, NULL);

	uint8_t pkt[188];
	memset(pkt, 0xff, sizeof(pkt));
	int complete = -1;
	struct timeval ts = { 1, 0 };
	size_t ret = ltntstools_streammodel_write(hdl, pkt, 0, &complete, &ts);
	CHECK(ret == (size_t)-1);

	ltntstools_streammodel_free(hdl);
}

static void test_query_model_before_write_fails(void)
{
	void *hdl = NULL;
	ltntstools_streammodel_alloc(&hdl, NULL);

	struct ltntstools_pat_s *pat = NULL;
	CHECK(ltntstools_streammodel_query_model(hdl, &pat) == -1);

	ltntstools_streammodel_free(hdl);
}

/* -------- end-to-end: real PAT/PMT decode via libdvbpsi -------- */

static void test_write_spts_completes_model_and_query_matches(void)
{
	void *hdl = NULL;
	ltntstools_streammodel_alloc(&hdl, NULL);

	/* Build a 1-program PAT pointing at PMT pid 0x100, whose PMT carries
	 * a video (0x101) and an audio (0x102) stream, PCR on the video pid. */
	struct ltntstools_pat_s *inPat = ltntstools_pat_alloc();
	inPat->transport_stream_id = 0x1234;
	struct ltntstools_pat_program_s *pp = add_program(inPat, 1, 0x100);
	pp->pmt.PCR_PID = 0x101;
	add_stream(&pp->pmt, 0x1b /* AVC */, 0x101);
	add_stream(&pp->pmt, 0x0f /* AAC */, 0x102);

	uint8_t buf[3][188];
	CHECK(ltntstools_pat_create_packet_ts(inPat, 0, buf[0], 188) == 0);
	CHECK(ltntstools_pmt_create_packet_ts(&pp->pmt, 0x100, 0, buf[1], 188) == 0);

	struct timeval ts = { 1, 0 };
	int complete = -1;
	size_t ret = ltntstools_streammodel_write(hdl, &buf[0][0], 2, &complete, &ts);

	CHECK(ret == 2);
	CHECK(complete == 1);
	CHECK(ltntstools_streammodel_get_current_version(hdl) == 1);

	struct ltntstools_pat_s *outPat = NULL;
	CHECK(ltntstools_streammodel_query_model(hdl, &outPat) == 0);
	CHECK(outPat != NULL);

	if (outPat) {
		CHECK(outPat->program_count == 1);
		CHECK(outPat->programs[0].program_number == 1);
		CHECK(outPat->programs[0].program_map_PID == 0x100);
		CHECK(outPat->programs[0].pmt.program_number == 1);
		CHECK(outPat->programs[0].pmt.PCR_PID == 0x101);
		CHECK(outPat->programs[0].pmt.stream_count == 2);

		int sawVideo = 0, sawAudio = 0;
		for (uint32_t i = 0; i < outPat->programs[0].pmt.stream_count; i++) {
			struct ltntstools_pmt_entry_s *e = &outPat->programs[0].pmt.streams[i];
			if (e->stream_type == 0x1b && e->elementary_PID == 0x101)
				sawVideo = 1;
			if (e->stream_type == 0x0f && e->elementary_PID == 0x102)
				sawAudio = 1;
		}
		CHECK(sawVideo);
		CHECK(sawAudio);

		CHECK(ltntstools_streammodel_is_model_mpts(hdl, outPat) == 0);

		uint16_t pcrpid = 0;
		CHECK(ltntstools_streammodel_query_first_program_pcr_pid(hdl, outPat, &pcrpid) == 0);
		CHECK(pcrpid == 0x101);

		ltntstools_pat_free(outPat);
	}

	ltntstools_pat_free(inPat);
	ltntstools_streammodel_free(hdl);
}

static void test_write_mpts_completes_model_with_two_programs(void)
{
	void *hdl = NULL;
	ltntstools_streammodel_alloc(&hdl, NULL);

	struct ltntstools_pat_s *inPat = ltntstools_pat_alloc();
	inPat->transport_stream_id = 0x5678;

	struct ltntstools_pat_program_s *pp1 = add_program(inPat, 1, 0x100);
	pp1->pmt.PCR_PID = 0x101;
	add_stream(&pp1->pmt, 0x1b, 0x101);

	struct ltntstools_pat_program_s *pp2 = add_program(inPat, 2, 0x200);
	pp2->pmt.PCR_PID = 0x201;
	add_stream(&pp2->pmt, 0x1b, 0x201);

	uint8_t buf[3][188];
	CHECK(ltntstools_pat_create_packet_ts(inPat, 0, buf[0], 188) == 0);
	CHECK(ltntstools_pmt_create_packet_ts(&pp1->pmt, 0x100, 0, buf[1], 188) == 0);
	CHECK(ltntstools_pmt_create_packet_ts(&pp2->pmt, 0x200, 0, buf[2], 188) == 0);

	struct timeval ts = { 1, 0 };
	int complete = -1;
	size_t ret = ltntstools_streammodel_write(hdl, &buf[0][0], 3, &complete, &ts);

	CHECK(ret == 3);
	CHECK(complete == 1);

	struct ltntstools_pat_s *outPat = NULL;
	CHECK(ltntstools_streammodel_query_model(hdl, &outPat) == 0);

	if (outPat) {
		CHECK(outPat->program_count == 2);
		CHECK(ltntstools_streammodel_is_model_mpts(hdl, outPat) == 1);
		ltntstools_pat_free(outPat);
	}

	ltntstools_pat_free(inPat);
	ltntstools_streammodel_free(hdl);
}

static void test_write_no_pat_yet_leaves_model_incomplete(void)
{
	/* A lone, non-PAT/PMT packet (junk pid) shouldn't complete a model. */
	void *hdl = NULL;
	ltntstools_streammodel_alloc(&hdl, NULL);

	uint8_t pkt[188];
	memset(pkt, 0xff, sizeof(pkt));
	pkt[0] = 0x47;
	pkt[1] = 0x41; /* pid 0x100, no payload_unit_start */
	pkt[2] = 0x00;
	pkt[3] = 0x10;

	struct timeval ts = { 1, 0 };
	int complete = -1;
	ltntstools_streammodel_write(hdl, pkt, 1, &complete, &ts);

	CHECK(complete == 0);
	CHECK(ltntstools_streammodel_get_current_version(hdl) == 0);

	struct ltntstools_pat_s *outPat = NULL;
	CHECK(ltntstools_streammodel_query_model(hdl, &outPat) == -1);

	ltntstools_streammodel_free(hdl);
}

int main(void)
{
	test_is_model_mpts_single_program_is_spts();
	test_is_model_mpts_two_programs_is_mpts();
	test_is_model_mpts_ignores_network_pid_program_zero();

	test_query_first_program_pcr_pid_null_args_fail();
	test_query_first_program_pcr_pid_returns_first_match();
	test_query_first_program_pcr_pid_skips_streamless_programs();
	test_query_first_program_pcr_pid_no_pcr_set_fails();

	test_alloc_free_basic();
	test_get_current_version_starts_at_zero();

	test_write_rejects_null_hdl();
	test_write_rejects_null_packet_buffer();
	test_write_rejects_non_positive_packet_count();
	test_query_model_before_write_fails();

	test_write_spts_completes_model_and_query_matches();
	test_write_mpts_completes_model_with_two_programs();
	test_write_no_pat_yet_leaves_model_incomplete();

	if (g_failures == 0) {
		printf("PASS: all streammodel tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d streammodel test(s) failed\n", g_failures);
	return 1;
}
