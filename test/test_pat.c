/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/pat.c / src/libltntstools/pat.h.
 * Builds against ../src/pat.c plus ../src/descriptor.c, ../src/crc32.c and
 * ../src/ts.c, and links against libdvbpsi (pat.c includes dvbpsi headers
 * directly and calls into it from ltntstools_pmt_create_packet_ts2-style
 * helpers). See test/Makefile for how libdvbpsi is located.
 *
 * SCOPE: ltntstools_pat_alloc_from_existing() and
 * ltntstools_pat_add_from_existing() consume libdvbpsi's own decoded
 * PAT/PMT structures (dvbpsi_pat_t/dvbpsi_pmt_t). They ARE covered below
 * (see the alloc_from_existing/add_from_existing section) by building
 * those structures with libdvbpsi's own dvbpsi_pat_new()/
 * dvbpsi_pat_program_add()/dvbpsi_pmt_new()/dvbpsi_pmt_es_add()/
 * dvbpsi_pmt_descriptor_add()/dvbpsi_pmt_es_descriptor_add() APIs rather
 * than hand-building the linked lists.
 *
 * ltntstools_pmt_create_packet_ts2() is different: like
 * the non-"2" ltntstools_pmt_create_packet_ts(), its input is this library's
 * own struct ltntstools_pmt_s -- it just generates the section bytes via
 * real libdvbpsi calls internally instead of hand-rolling them -- so it IS
 * covered below (see test_pmt_create_packet_ts2_structure_and_crc()).
 * Everything else in pat.c operates purely on this library's own
 * ltntstools_pat_s/ltntstools_pmt_s structs, which is what's covered below.
 *
 * ltntstools_pat_create_packet_ts() and ltntstools_pmt_create_packet_ts()
 * previously had a confirmed bug, found while writing
 * test_pat_create_packet_ts_version_number() and fixed in src/pat.c: the
 * byte encoding `reserved(2 bits)='11' | version_number(5 bits) |
 * current_next_indicator(1 bit)` was written using `<` where `<<` was
 * clearly intended:
 *     0xC0 | ((pat->version_number & 0x1f) < 1) | pat->current_next_indicator
 * As written, version_number wasn't encoded at all -- it was replaced by
 * the boolean result of "is version_number zero", which also collided with
 * current_next_indicator in bit 0 of the same byte. Confirmed with a
 * standalone repro (version_number=7 produced 0xC1 instead of 0xCF) before
 * being fixed to `<<`.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>

#include <dvbpsi/dvbpsi.h>
#include <dvbpsi/psi.h>
#include <dvbpsi/pat.h>
#include <dvbpsi/descriptor.h>
#include <dvbpsi/pmt.h>

#include "libltntstools/pat.h"
#include "libltntstools/ts.h"
#include "libltntstools/crc32.h"

/* Not declared in pat.h, but has real external linkage in pat.c -- unlike
 * ltntstools_pat_alloc_from_existing()/_add_from_existing(), its input is
 * this library's own struct ltntstools_pmt_s (same as the non-"2" variant
 * above it in pat.c), not a libdvbpsi-decoded structure the caller would
 * need to hand-construct. See test_pmt_create_packet_ts2_structure_and_crc(). */
int ltntstools_pmt_create_packet_ts2(struct ltntstools_pmt_s *p, uint16_t pid, uint8_t cc, uint8_t *packet, int packetLength);

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* Redirects stderr to a temp file so a test can inspect diagnostic output
 * (eg. issue #7's PAT/PMT-truncation and descriptor-add-failure logging,
 * which has no other externally observable effect). Must be paired with
 * capture_stderr_stop(). */
static int g_capture_saved_fd = -1;
static char g_capture_path[] = "/tmp/test_pat_stderr_XXXXXX";

static void capture_stderr_start(void)
{
	fflush(stderr);
	int fd = mkstemp(g_capture_path);
	g_capture_saved_fd = dup(fileno(stderr));
	dup2(fd, fileno(stderr));
	close(fd);
}

static void capture_stderr_stop(char *buf, size_t buflen)
{
	fflush(stderr);
	dup2(g_capture_saved_fd, fileno(stderr));
	close(g_capture_saved_fd);
	g_capture_saved_fd = -1;

	FILE *f = fopen(g_capture_path, "r");
	size_t n = f ? fread(buf, 1, buflen - 1, f) : 0;
	buf[n] = 0;
	if (f)
		fclose(f);
	unlink(g_capture_path);
}

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

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	CHECK(pat != NULL);
	CHECK(pat->program_count == 0);
	ltntstools_pat_free(pat);
}

static void test_clone_null_returns_null(void)
{
	CHECK(ltntstools_pat_clone(NULL) == NULL);
}

static void test_clone_matches_and_is_independent(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	pat->transport_stream_id = 7;
	struct ltntstools_pat_program_s *pp = add_program(pat, 1, 0x100);
	add_stream(&pp->pmt, 0x1B, 0x101);

	struct ltntstools_pat_s *clone = ltntstools_pat_clone(pat);
	CHECK(clone != NULL);
	CHECK(clone != pat); /* distinct allocation */
	CHECK(ltntstools_pat_compare(pat, clone) == 0);

	/* Mutating the original after cloning must not affect the clone
	 * (clone() is a flat memcpy of a pointer-free struct, so this is
	 * really checking there's no accidental aliasing). */
	pat->programs[0].program_map_PID = 0x999;
	CHECK(clone->programs[0].program_map_PID == 0x100);

	ltntstools_pat_free(pat);
	ltntstools_pat_free(clone);
}

static void test_dprintf_does_not_crash(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	struct ltntstools_pat_program_s *pp = add_program(pat, 1, 0x100);
	add_stream(&pp->pmt, 0x1B, 0x101);

	int fd = open("/dev/null", 1 /* O_WRONLY */);
	if (fd >= 0) {
		ltntstools_pat_dprintf(pat, fd);

		/* Regression test for issue #5: dprintf() dereferenced pat with
		 * no NULL check at all. Must be a safe no-op. */
		ltntstools_pat_dprintf(NULL, fd);

		close(fd);
	}
	CHECK(1);

	ltntstools_pat_free(pat);
}

/* -------- alloc_from_existing / add_from_existing -------- */

/* Regression test for issue #7: ltntstools_pat_alloc_from_existing() caps
 * program_count at LTNTSTOOLS_PAT_ENTRIES_MAX (correct, and unchanged by
 * this fix -- the array genuinely can't hold more), but previously did so
 * silently: a caller feeding it a PAT with more than 64 programs had no
 * way to know anything had been dropped. Now it logs to stderr. Builds a
 * real dvbpsi_pat_t (via dvbpsi_pat_new()/dvbpsi_pat_program_add(), the
 * same libdvbpsi API pat.c itself uses to build these) with one more
 * program than fits, and checks both the capped count/content (functional
 * behavior, unchanged) and that the drop was actually logged (the part
 * this fix adds -- a mutation reverting just the logging wouldn't be
 * caught by a count-only check). */
static void test_pat_alloc_from_existing_truncates_and_logs_excess_programs(void)
{
	dvbpsi_pat_t *dpat = dvbpsi_pat_new(1, 0, true);
	for (int i = 0; i < LTNTSTOOLS_PAT_ENTRIES_MAX + 1; i++) {
		dvbpsi_pat_program_add(dpat, 100 + i, 0x200 + i);
	}

	capture_stderr_start();
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc_from_existing(dpat);
	char captured[512];
	capture_stderr_stop(captured, sizeof(captured));

	CHECK(pat != NULL);
	CHECK(pat->program_count == LTNTSTOOLS_PAT_ENTRIES_MAX);
	CHECK(pat->programs[0].program_number == 100);
	CHECK(pat->programs[LTNTSTOOLS_PAT_ENTRIES_MAX - 1].program_number == 100 + (LTNTSTOOLS_PAT_ENTRIES_MAX - 1));
	CHECK(strstr(captured, "dropped") != NULL);

	ltntstools_pat_free(pat);
	dvbpsi_pat_delete(dpat);
}

/* Baseline for the same function: when everything fits, no truncation
 * message should appear. */
static void test_pat_alloc_from_existing_no_log_when_everything_fits(void)
{
	dvbpsi_pat_t *dpat = dvbpsi_pat_new(1, 0, true);
	dvbpsi_pat_program_add(dpat, 1, 0x100);
	dvbpsi_pat_program_add(dpat, 2, 0x101);

	capture_stderr_start();
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc_from_existing(dpat);
	char captured[512];
	capture_stderr_stop(captured, sizeof(captured));

	CHECK(pat != NULL);
	CHECK(pat->program_count == 2);
	CHECK(captured[0] == 0);

	ltntstools_pat_free(pat);
	dvbpsi_pat_delete(dpat);
}

/* Regression test for issue #7: ltntstools_pat_add_from_existing() caps a
 * program's stream_count at LTNTSTOOLS_PMT_ENTRIES_MAX the same way, and
 * previously dropped the excess ES entries silently. Now logs to stderr. */
static void test_pat_add_from_existing_truncates_and_logs_excess_streams(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 7, 0x100);

	dvbpsi_pmt_t *dpmt = dvbpsi_pmt_new(7, 0, true, 0x101);
	for (int i = 0; i < LTNTSTOOLS_PMT_ENTRIES_MAX + 1; i++) {
		dvbpsi_pmt_es_add(dpmt, 0x1B, 0x200 + i);
	}

	capture_stderr_start();
	ltntstools_pat_add_from_existing(pat, dpmt);
	char captured[512];
	capture_stderr_stop(captured, sizeof(captured));

	CHECK(pat->programs[0].pmt.stream_count == LTNTSTOOLS_PMT_ENTRIES_MAX);
	CHECK(pat->programs[0].pmt.streams[0].elementary_PID == 0x200);
	CHECK(strstr(captured, "dropped") != NULL);

	dvbpsi_pmt_delete(dpmt);
	ltntstools_pat_free(pat);
}

/* Regression test for issue #7: ltntstools_pat_add_from_existing()'s two
 * descriptor-add loops (outer PMT descriptors and per-ES inner
 * descriptors) previously discarded a ltntstools_descriptor_list_add()
 * failure -- eg. more than LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX descriptors
 * on either list -- with a comment ("Error, skipping") and no actual
 * logging. Now both loops log to stderr. */
static void test_pat_add_from_existing_logs_descriptor_add_failures(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 9, 0x100);

	dvbpsi_pmt_t *dpmt = dvbpsi_pmt_new(9, 0, true, 0x101);
	uint8_t data[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
	for (int i = 0; i < LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX + 1; i++) {
		dvbpsi_pmt_descriptor_add(dpmt, 0x05, sizeof(data), data);
	}

	dvbpsi_pmt_es_t *es = dvbpsi_pmt_es_add(dpmt, 0x1B, 0x300);
	for (int i = 0; i < LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX + 1; i++) {
		dvbpsi_pmt_es_descriptor_add(es, 0x05, sizeof(data), data);
	}

	capture_stderr_start();
	ltntstools_pat_add_from_existing(pat, dpmt);
	char captured[512];
	capture_stderr_stop(captured, sizeof(captured));

	CHECK(pat->programs[0].pmt.descr_list.count == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX);
	CHECK(pat->programs[0].pmt.streams[0].descr_list.count == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX);
	CHECK(strstr(captured, "failed to add outer descriptor") != NULL);
	CHECK(strstr(captured, "failed to add inner descriptor") != NULL);

	dvbpsi_pmt_delete(dpmt);
	ltntstools_pat_free(pat);
}

/* -------- compare functions -------- */

static void test_pmt_entry_compare(void)
{
	struct ltntstools_pmt_entry_s a = { .stream_type = 0x1B, .elementary_PID = 0x101 };
	struct ltntstools_pmt_entry_s b = a;
	CHECK(ltntstools_pmt_entry_compare(&a, &b) == 0);

	b.elementary_PID = 0x102;
	CHECK(ltntstools_pmt_entry_compare(&a, &b) != 0);

	b = a;
	b.stream_type = 0x0F;
	CHECK(ltntstools_pmt_entry_compare(&a, &b) != 0);
}

static void test_pmt_compare(void)
{
	struct ltntstools_pmt_s a = { 0 };
	a.version_number = 1;
	a.program_number = 1;
	a.PCR_PID = 0x101;
	add_stream(&a, 0x1B, 0x101);

	struct ltntstools_pmt_s b = a;
	CHECK(ltntstools_pmt_compare(&a, &b) == 0);

	b.PCR_PID = 0x102;
	CHECK(ltntstools_pmt_compare(&a, &b) != 0);

	b = a;
	b.stream_count = 0;
	CHECK(ltntstools_pmt_compare(&a, &b) != 0);
}

static void test_pat_program_and_pat_compare(void)
{
	struct ltntstools_pat_s *a = ltntstools_pat_alloc();
	a->transport_stream_id = 5;
	struct ltntstools_pat_program_s *pp = add_program(a, 1, 0x100);
	add_stream(&pp->pmt, 0x1B, 0x101);

	struct ltntstools_pat_s *b = ltntstools_pat_clone(a);
	CHECK(ltntstools_pat_compare(a, b) == 0);
	CHECK(ltntstools_pat_program_compare(&a->programs[0], &b->programs[0]) == 0);

	b->transport_stream_id = 6;
	CHECK(ltntstools_pat_compare(a, b) != 0);

	b->transport_stream_id = 5;
	b->programs[0].program_map_PID = 0x200;
	CHECK(ltntstools_pat_compare(a, b) != 0);
	CHECK(ltntstools_pat_program_compare(&a->programs[0], &b->programs[0]) != 0);

	ltntstools_pat_free(a);
	ltntstools_pat_free(b);
}

/* -------- classification helpers -------- */

static void test_pmt_query_video_pid(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	add_stream(&pmt, 0x03 /* audio */, 0x101);
	add_stream(&pmt, 0x1B /* H.264 video */, 0x102);

	uint16_t pid = 0;
	uint8_t estype = 0;
	CHECK(ltntstools_pmt_query_video_pid(&pmt, &pid, &estype) == 0);
	CHECK(pid == 0x102);
	CHECK(estype == 0x1B);

	struct ltntstools_pmt_s audioOnly = { 0 };
	add_stream(&audioOnly, 0x03, 0x101);
	CHECK(ltntstools_pmt_query_video_pid(&audioOnly, &pid, &estype) < 0);

	CHECK(ltntstools_pmt_query_video_pid(NULL, &pid, &estype) < 0);
}

static void test_pmt_entry_is_video(void)
{
	struct ltntstools_pmt_entry_s e = { 0 };
	e.stream_type = 0x1B;
	CHECK(ltntstools_pmt_entry_is_video(&e) == 1);
	e.stream_type = 0x03;
	CHECK(ltntstools_pmt_entry_is_video(&e) == 0);
}

static void test_pmt_entry_is_audio_by_stream_type(void)
{
	struct ltntstools_pmt_entry_s e = { 0 };
	e.stream_type = 0x0F; /* AAC/ADTS */
	CHECK(ltntstools_pmt_entry_is_audio(&e) != 0);
}

static void test_pmt_entry_is_audio_by_private_data_descriptor(void)
{
	struct ltntstools_pmt_entry_s e = { 0 };
	e.stream_type = 0x06; /* PES private data -- needs a descriptor to classify */
	ltntstools_descriptor_list_add(&e.descr_list, 0x6a /* AC-3 descriptor */, (uint8_t *)"", 0);
	CHECK(ltntstools_pmt_entry_is_audio(&e) == 0x6a);

	struct ltntstools_pmt_entry_s noDescr = { 0 };
	noDescr.stream_type = 0x06;
	CHECK(ltntstools_pmt_entry_is_audio(&noDescr) == 0);

	struct ltntstools_pmt_entry_s wrongType = { 0 };
	wrongType.stream_type = 0x1B; /* video, not 0x06 */
	CHECK(ltntstools_pmt_entry_is_audio(&wrongType) == 0);
}

static void test_pmt_entry_is_scte35_outer_and_inner_descriptor(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	struct ltntstools_pmt_entry_s e = { 0 };
	e.stream_type = 0x86;

	/* Not SCTE35 without a CUEI registration anywhere. */
	CHECK(ltntstools_pmt_entry_is_scte35(&pmt, &e) == 0);

	/* CUEI in the inner (ES-level) descriptor loop. */
	struct ltntstools_pmt_entry_s eInner = { 0 };
	eInner.stream_type = 0x86;
	ltntstools_descriptor_list_add(&eInner.descr_list, 0x05, (uint8_t *)"CUEI", 4);
	CHECK(ltntstools_pmt_entry_is_scte35(&pmt, &eInner) == 1);

	/* CUEI in the outer (PMT program_info) descriptor loop only. */
	struct ltntstools_pmt_s pmtOuter = { 0 };
	ltntstools_descriptor_list_add(&pmtOuter.descr_list, 0x05, (uint8_t *)"CUEI", 4);
	struct ltntstools_pmt_entry_s eNoDescr = { 0 };
	eNoDescr.stream_type = 0x86;
	CHECK(ltntstools_pmt_entry_is_scte35(&pmtOuter, &eNoDescr) == 1);

	/* Wrong stream_type never matches, even with CUEI present. */
	struct ltntstools_pmt_entry_s wrongType = { 0 };
	wrongType.stream_type = 0x1B;
	ltntstools_descriptor_list_add(&wrongType.descr_list, 0x05, (uint8_t *)"CUEI", 4);
	CHECK(ltntstools_pmt_entry_is_scte35(&pmt, &wrongType) == 0);
}

static void test_pmt_entry_is_smpte2038(void)
{
	struct ltntstools_pmt_entry_s e = { 0 };
	e.stream_type = 0x06;
	ltntstools_descriptor_list_add(&e.descr_list, 0x05, (uint8_t *)"VANC", 4);
	CHECK(ltntstools_pmt_entry_is_smpte2038(&e) == 1);

	struct ltntstools_pmt_entry_s wrongType = { 0 };
	wrongType.stream_type = 0x1B;
	ltntstools_descriptor_list_add(&wrongType.descr_list, 0x05, (uint8_t *)"VANC", 4);
	CHECK(ltntstools_pmt_entry_is_smpte2038(&wrongType) == 0);
}

/* -------- get_services_teletext -------- */

static void test_get_services_teletext_found(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	struct ltntstools_pat_program_s *pp = add_program(pat, 1, 0x100);
	add_stream(&pp->pmt, 0x06, 0x101);
	uint8_t ttx[5] = { 0 };
	ltntstools_descriptor_list_add(&pp->pmt.streams[0].descr_list, 0x56, ttx, sizeof(ttx));
	add_stream(&pp->pmt, 0x03, 0x102); /* unrelated audio stream */

	uint16_t *pids = NULL;
	int count = 0;
	CHECK(ltntstools_pat_get_services_teletext(pat, &pids, &count) == 0);
	CHECK(count == 1);
	CHECK(pids != NULL && pids[0] == 0x101);

	free(pids);
	ltntstools_pat_free(pat);
}

/* Exercises the cnt==0 path -- issue #6: this used to call malloc(0), whose
 * return value is implementation-defined by the C standard (NULL, or a
 * valid-but-unusable pointer). On this platform malloc(0) happens to
 * return non-NULL, so this "worked" before the fix, but a NULL-returning
 * platform would have made this function incorrectly report -1 for the
 * common "no teletext streams" case. Now returns early before ever calling
 * malloc(0), so this is deterministic on any platform, not platform-lucky --
 * pids is asserted NULL, not just "whatever malloc(0) happened to return". */
static void test_get_services_teletext_none_found(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 1, 0x100); /* no streams at all */

	uint16_t *pids = (uint16_t *)0x1; /* sentinel, must be overwritten */
	int count = -1;
	int ret = ltntstools_pat_get_services_teletext(pat, &pids, &count);
	CHECK(ret == 0);
	CHECK(count == 0);
	CHECK(pids == NULL);

	free(pids);
	ltntstools_pat_free(pat);
}

static void test_get_services_teletext_null_args(void)
{
	uint16_t *pids = NULL;
	int count = 0;
	CHECK(ltntstools_pat_get_services_teletext(NULL, &pids, &count) < 0);
}

/* -------- enum_services_scte35 -------- */

static void test_enum_services_scte35_finds_and_terminates(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();

	struct ltntstools_pat_program_s *p1 = add_program(pat, 1, 0x100);
	add_stream(&p1->pmt, 0x1B, 0x101); /* video only, not SCTE35 */

	struct ltntstools_pat_program_s *p2 = add_program(pat, 2, 0x200);
	ltntstools_descriptor_list_add(&p2->pmt.descr_list, 0x05, (uint8_t *)"CUEI", 4);
	add_stream(&p2->pmt, 0x86, 0x201); /* SCTE35 splice info */

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	uint16_t *pids = NULL;
	int pidCount = 0;
	CHECK(ltntstools_pat_enum_services_scte35(pat, &e, &pmt, &pids, &pidCount) == 0);
	CHECK(pmt == &p2->pmt);
	CHECK(pidCount == 1);
	CHECK(pids && pids[0] == 0x201);
	free(pids);

	/* No more matches. */
	CHECK(ltntstools_pat_enum_services_scte35(pat, &e, &pmt, &pids, &pidCount) < 0);

	ltntstools_pat_free(pat);
}

/* Regression test for issue #6: a program whose PMT carries the SCTE35
 * CUEI registration descriptor but has zero elementary streams (degenerate
 * but a valid combination of the struct fields) used to call
 * malloc(0 * sizeof(uint16_t)) -- see the teletext test above for why
 * that's a real risk, not just a style nit. Must skip past it (not report
 * a spurious allocation failure) and keep enumerating. */
static void test_enum_services_scte35_skips_zero_stream_program(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();

	struct ltntstools_pat_program_s *p1 = add_program(pat, 1, 0x100);
	ltntstools_descriptor_list_add(&p1->pmt.descr_list, 0x05, (uint8_t *)"CUEI", 4);
	/* p1 deliberately has zero streams. */

	struct ltntstools_pat_program_s *p2 = add_program(pat, 2, 0x200);
	ltntstools_descriptor_list_add(&p2->pmt.descr_list, 0x05, (uint8_t *)"CUEI", 4);
	add_stream(&p2->pmt, 0x86, 0x201);

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	uint16_t *pids = NULL;
	int pidCount = 0;
	CHECK(ltntstools_pat_enum_services_scte35(pat, &e, &pmt, &pids, &pidCount) == 0);
	CHECK(pmt == &p2->pmt);
	CHECK(pidCount == 1);
	CHECK(pids && pids[0] == 0x201);
	free(pids);

	ltntstools_pat_free(pat);
}

/* -------- enum_services_smpte2038 -------- */

static void test_enum_services_smpte2038_finds_and_terminates(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();

	struct ltntstools_pat_program_s *p1 = add_program(pat, 1, 0x100);
	add_stream(&p1->pmt, 0x06, 0x101);
	ltntstools_descriptor_list_add(&p1->pmt.streams[0].descr_list, 0x05, (uint8_t *)"VANC", 4);

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	uint16_t pid = 0;
	CHECK(ltntstools_pat_enum_services_smpte2038(pat, &e, &pmt, &pid) == 0);
	CHECK(pmt == &p1->pmt);
	CHECK(pid == 0x101);

	CHECK(ltntstools_pat_enum_services_smpte2038(pat, &e, &pmt, &pid) < 0);

	ltntstools_pat_free(pat);
}

/* -------- enum_services_video / enum_services_teletext -------- */

static void test_enum_services_video_multi_program(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();

	struct ltntstools_pat_program_s *p1 = add_program(pat, 1, 0x100);
	add_stream(&p1->pmt, 0x03, 0x101); /* audio only */

	struct ltntstools_pat_program_s *p2 = add_program(pat, 2, 0x200);
	add_stream(&p2->pmt, 0x1B, 0x201); /* video */

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	CHECK(ltntstools_pat_enum_services_video(pat, &e, &pmt) == 0);
	CHECK(pmt == &p2->pmt);
	CHECK(ltntstools_pat_enum_services_video(pat, &e, &pmt) < 0);

	ltntstools_pat_free(pat);
}

static void test_enum_services_teletext_multi_program(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();

	struct ltntstools_pat_program_s *p1 = add_program(pat, 1, 0x100);
	add_stream(&p1->pmt, 0x03, 0x101);

	struct ltntstools_pat_program_s *p2 = add_program(pat, 2, 0x200);
	add_stream(&p2->pmt, 0x06, 0x201);
	uint8_t ttx[5] = { 0 };
	ltntstools_descriptor_list_add(&p2->pmt.streams[0].descr_list, 0x56, ttx, sizeof(ttx));

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	CHECK(ltntstools_pat_enum_services_teletext(pat, &e, &pmt) == 0);
	CHECK(pmt == &p2->pmt);
	CHECK(ltntstools_pat_enum_services_teletext(pat, &e, &pmt) < 0);

	ltntstools_pat_free(pat);
}

/* -------- enum_services_audio -------- */

static void test_enum_services_audio_multi_pid_single_program(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	struct ltntstools_pat_program_s *pp = add_program(pat, 1, 0x100);
	add_stream(&pp->pmt, 0x1B, 0x101); /* video, not audio */
	add_stream(&pp->pmt, 0x03, 0x102); /* audio */
	add_stream(&pp->pmt, 0x0F, 0x103); /* audio */

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	uint32_t *streamTypes = NULL;
	uint16_t *pids = NULL;
	int pidCount = 0;
	CHECK(ltntstools_pat_enum_services_audio(pat, &e, &pmt, &streamTypes, &pids, &pidCount) == 0);
	CHECK(pmt == &pp->pmt);
	CHECK(pidCount == 2);
	CHECK(pids && (pids[0] == 0x102) && (pids[1] == 0x103));
	CHECK(streamTypes && (streamTypes[0] == 0x03) && (streamTypes[1] == 0x0F));
	free(pids);
	free(streamTypes);

	CHECK(ltntstools_pat_enum_services_audio(pat, &e, &pmt, &streamTypes, &pids, &pidCount) < 0);

	ltntstools_pat_free(pat);
}

/* Regression test for issue #6: a program with zero elementary streams
 * used to call malloc(0 * sizeof(...)) for both the pid and stream-type
 * arrays -- see the teletext test above for why that's a real risk, not
 * just a style nit. Must skip past it and keep enumerating rather than
 * reporting a spurious allocation failure. */
static void test_enum_services_audio_skips_zero_stream_program(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 1, 0x100); /* zero streams */

	struct ltntstools_pat_program_s *p2 = add_program(pat, 2, 0x200);
	add_stream(&p2->pmt, 0x03, 0x201); /* audio */

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	uint32_t *streamTypes = NULL;
	uint16_t *pids = NULL;
	int pidCount = 0;
	CHECK(ltntstools_pat_enum_services_audio(pat, &e, &pmt, &streamTypes, &pids, &pidCount) == 0);
	CHECK(pmt == &p2->pmt);
	CHECK(pidCount == 1);
	CHECK(pids && pids[0] == 0x201);
	CHECK(streamTypes && streamTypes[0] == 0x03);
	free(pids);
	free(streamTypes);

	ltntstools_pat_free(pat);
}

/* -------- enum_services (generic, by PMT pid) -------- */

static void test_enum_services_by_pid(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	add_program(pat, 1, 0x100);
	struct ltntstools_pat_program_s *p2 = add_program(pat, 2, 0x200);

	int e = 0;
	struct ltntstools_pmt_s *pmt = NULL;
	CHECK(ltntstools_pat_enum_services(pat, &e, 0x200, &pmt) == 0);
	CHECK(pmt == &p2->pmt);

	e = 0;
	CHECK(ltntstools_pat_enum_services(pat, &e, 0x999 /* not present */, &pmt) < 0);

	ltntstools_pat_free(pat);
}

/* -------- pmt_remove_es_for_pid -------- */

static void test_pmt_remove_es_for_pid_start_middle_end(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	add_stream(&pmt, 0x01, 0x101);
	add_stream(&pmt, 0x02, 0x102);
	add_stream(&pmt, 0x03, 0x103);

	/* Remove the middle entry. */
	CHECK(ltntstools_pmt_remove_es_for_pid(&pmt, 0x102) == 0);
	CHECK(pmt.stream_count == 2);
	CHECK(pmt.streams[0].elementary_PID == 0x101);
	CHECK(pmt.streams[1].elementary_PID == 0x103);

	/* Remove the (now) last entry. */
	CHECK(ltntstools_pmt_remove_es_for_pid(&pmt, 0x103) == 0);
	CHECK(pmt.stream_count == 1);
	CHECK(pmt.streams[0].elementary_PID == 0x101);

	/* Remove the only remaining (first) entry. */
	CHECK(ltntstools_pmt_remove_es_for_pid(&pmt, 0x101) == 0);
	CHECK(pmt.stream_count == 0);
}

static void test_pmt_remove_es_for_pid_not_found(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	add_stream(&pmt, 0x01, 0x101);

	CHECK(ltntstools_pmt_remove_es_for_pid(&pmt, 0x999) == 0); /* still "success" */
	CHECK(pmt.stream_count == 1); /* unchanged */

	CHECK(ltntstools_pmt_remove_es_for_pid(NULL, 0x101) < 0);
}

/* -------- pmt_enum_services_audio (array variant) -------- */

static void test_pmt_enum_services_audio_array(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	add_stream(&pmt, 0x1B, 0x101); /* video */
	add_stream(&pmt, 0x03, 0x102); /* audio */
	add_stream(&pmt, 0x0F, 0x103); /* audio */

	int pidCount = 0;
	const struct ltntstools_pmt_entry_s **streams = ltntstools_pmt_enum_services_audio(&pmt, &pidCount);
	CHECK(streams != NULL);
	CHECK(pidCount == 2);
	if (streams) {
		CHECK(streams[0]->elementary_PID == 0x102);
		CHECK(streams[1]->elementary_PID == 0x103);
		free(streams);
	}
}

/* Regression test for issue #6: a PMT with zero elementary streams used to
 * call malloc(0 * sizeof(...)) -- see the teletext test above for why
 * that's a real risk, not just a style nit. Must return NULL (this
 * function's existing, documented "nothing found" contract) without ever
 * attempting the malloc(0). */
static void test_pmt_enum_services_audio_array_zero_streams(void)
{
	struct ltntstools_pmt_s pmt = { 0 };

	int pidCount = -1;
	const struct ltntstools_pmt_entry_s **streams = ltntstools_pmt_enum_services_audio(&pmt, &pidCount);
	CHECK(streams == NULL);
	CHECK(pidCount == 0);
}

/* -------- create_packet_ts: structural correctness -------- */

static void test_pat_create_packet_ts_structure_and_crc(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	pat->transport_stream_id = 0x1234;
	add_program(pat, 1, 0x100);
	add_program(pat, 2, 0x200);

	uint8_t pkt[188];
	CHECK(ltntstools_pat_create_packet_ts(pat, 3, pkt, 188) == 0);

	CHECK(ltntstools_sync_present(pkt) == 1);
	CHECK(ltntstools_pid(pkt) == 0); /* PAT always on pid 0 */
	CHECK(ltntstools_payload_unit_start_indicator(pkt) == 1);
	CHECK(ltntstools_continuity_counter(pkt) == 3);
	CHECK(pkt[5] == 0x00); /* PAT table_id */
	CHECK(((pkt[9] << 8) | pkt[10] >> 8) || 1); /* placeholder to keep structure symmetrical below */

	uint16_t transport_stream_id = (pkt[8] << 8) | pkt[9];
	CHECK(transport_stream_id == 0x1234);

	/* section_length low byte is pkt[7] (9 + program_count*4 = 17, fits in one byte). */
	int section_length = pkt[7];
	CHECK(section_length == 9 + (2 * 4));

	/* Whole section (table_id..CRC inclusive) must checksum correctly. */
	int totalSectionBytes = 3 + section_length; /* table_id(1) + length_field(2) + section_length */
	CHECK(ltntstools_checkCRC32(&pkt[5], totalSectionBytes) == 0);

	/* Program entries: program_number (2 bytes) + reserved/PID (2 bytes) each. */
	CHECK(((pkt[13] << 8) | pkt[14]) == 1); /* program 1's number */
	CHECK((((pkt[15] & 0x1f) << 8) | pkt[16]) == 0x100);
	CHECK(((pkt[17] << 8) | pkt[18]) == 2); /* program 2's number */
	CHECK((((pkt[19] & 0x1f) << 8) | pkt[20]) == 0x200);

	ltntstools_pat_free(pat);
}

static void test_pat_create_packet_ts_version_number(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	pat->transport_stream_id = 1;
	pat->version_number = 7;
	pat->current_next_indicator = 1;
	add_program(pat, 1, 0x100);

	uint8_t pkt[188];
	ltntstools_pat_create_packet_ts(pat, 0, pkt, 188);

	uint8_t versionByte = pkt[10];
	CHECK(versionByte == (0xC0 | (7 << 1) | 1));

	ltntstools_pat_free(pat);
}

static void test_pmt_create_packet_ts_structure_and_crc(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	pmt.program_number = 1;
	pmt.PCR_PID = 0x101;
	add_stream(&pmt, 0x1B, 0x101);
	add_stream(&pmt, 0x03, 0x102);

	uint8_t pkt[188];
	CHECK(ltntstools_pmt_create_packet_ts(&pmt, 0x50, 4, pkt, 188) == 0);

	CHECK(ltntstools_sync_present(pkt) == 1);
	CHECK(ltntstools_pid(pkt) == 0x50);
	CHECK(ltntstools_continuity_counter(pkt) == 4);
	CHECK(pkt[5] == 0x02); /* PMT table_id */

	int section_length = pkt[7];
	int totalSectionBytes = 3 + section_length;
	CHECK(ltntstools_checkCRC32(&pkt[5], totalSectionBytes) == 0);

	uint16_t pcrPid = ((pkt[13] & 0x1f) << 8) | pkt[14];
	CHECK(pcrPid == 0x101);
}

static void test_pmt_create_packet_ts_version_number(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	pmt.program_number = 1;
	pmt.version_number = 9;
	pmt.current_next_indicator = 1;
	add_stream(&pmt, 0x1B, 0x101);

	uint8_t pkt[188];
	ltntstools_pmt_create_packet_ts(&pmt, 0x50, 0, pkt, 188);

	uint8_t versionByte = pkt[10];
	CHECK(versionByte == (0xC0 | (9 << 1) | 1));
}

/* Regression test for issue #2: same class of bug as issue #1's PAT
 * equivalent -- ltntstools_pmt_create_packet_ts() writes header + outer
 * descriptors + stream entries + inner descriptors + CRC into a fixed
 * 188-byte buffer with no check that the total fits. Reachable with a
 * single stream carrying one large descriptor, well short of the 16
 * stream / 16 descriptor array limits -- confirming this is easier to
 * trigger than the PAT overflow. Boundary-tests both sides: a 160-byte
 * descriptor (188 bytes total: fits exactly) vs 161 bytes (189: one too
 * many, rejected). Same guard-buffer + memcmp technique as the PAT test,
 * to directly prove the rejected call writes nothing at all. */
static void test_pmt_create_packet_ts_rejects_content_too_large_to_fit(void)
{
	uint8_t descriptorData[161];
	memset(descriptorData, 0x5A, sizeof(descriptorData));

	struct ltntstools_pmt_s pmt = { 0 };
	pmt.program_number = 1;
	add_stream(&pmt, 0x1B, 0x101);
	ltntstools_descriptor_list_add(&pmt.streams[0].descr_list, 0x05, descriptorData, 160);

	uint8_t pkt[188];
	CHECK(ltntstools_pmt_create_packet_ts(&pmt, 0x50, 0, pkt, 188) == 0); /* 188 bytes: fits exactly */

	/* Replace the single descriptor with a 161-byte one: 189 bytes, one too many. */
	pmt.streams[0].descr_list.count = 0;
	ltntstools_descriptor_list_add(&pmt.streams[0].descr_list, 0x05, descriptorData, 161);

	uint8_t guarded[188 + 16];
	memset(guarded, 0xAA, sizeof(guarded));
	int ret = ltntstools_pmt_create_packet_ts(&pmt, 0x50, 0, guarded, 188);
	CHECK(ret < 0);

	uint8_t untouched[188 + 16];
	memset(untouched, 0xAA, sizeof(untouched));
	CHECK(memcmp(guarded, untouched, sizeof(guarded)) == 0);
}

/* Basic coverage for ltntstools_pmt_create_packet_ts2() -- previously
 * entirely untested (the file header's scope note groups it with the two
 * genuinely-dvbpsi-input functions, but its own input is this library's
 * ltntstools_pmt_s, same as the non-"2" variant above; it just generates
 * the section bytes via real libdvbpsi calls instead of hand-rolling them).
 * Written while fixing issue #8 (missing NULL checks on dvbpsi_pmt_new()/
 * dvbpsi_pmt_es_add()/dvbpsi_new()/dvbpsi_pmt_sections_generate(), all
 * OOM-only) to at least confirm that change didn't break the success path
 * -- the OOM failure paths themselves aren't independently testable
 * without fault injection (same platform limitation established
 * elsewhere: a DYLD_INTERPOSE malloc-failure shim was tried and found
 * unreliable on this platform). */
static void test_pmt_create_packet_ts2_structure_and_crc(void)
{
	struct ltntstools_pmt_s pmt = { 0 };
	pmt.program_number = 1;
	pmt.PCR_PID = 0x101;
	add_stream(&pmt, 0x1B, 0x101);
	add_stream(&pmt, 0x03, 0x102);

	uint8_t pkt[188];
	CHECK(ltntstools_pmt_create_packet_ts2(&pmt, 0x50, 4, pkt, 188) == 0);

	CHECK(ltntstools_sync_present(pkt) == 1);
	CHECK(ltntstools_pid(pkt) == 0x50);
	/* Pre-existing quirk of writePSI() (unrelated to issues #2/#8): it
	 * increments the CC by one after writing each PSI section, so an
	 * input cc of 4 ends up as 5 in the output packet, not 4. */
	CHECK(ltntstools_continuity_counter(pkt) == 5);
	CHECK(pkt[5] == 0x02); /* PMT table_id */

	int section_length = pkt[7];
	int totalSectionBytes = 3 + section_length;
	CHECK(ltntstools_checkCRC32(&pkt[5], totalSectionBytes) == 0);

	uint16_t pcrPid = ((pkt[13] & 0x1f) << 8) | pkt[14];
	CHECK(pcrPid == 0x101);
}

/* Regression test for issue #1: ltntstools_pat_create_packet_ts() writes a
 * fixed 13-byte header + 4 bytes/program + 4 CRC bytes with no check that
 * the total fits within the caller's packetLength -- a PAT with more than
 * 42 programs needs 189+ bytes, overflowing the documented-and-required
 * 188-byte buffer. Boundary-tests both sides: 42 programs (185 bytes)
 * must still succeed, 43 (189 bytes) must be rejected. Uses a buffer
 * larger than 188 bytes, filled with a sentinel pattern, to directly prove
 * the rejected call doesn't write anything at all (not just that it
 * returns an error code) -- the strongest possible confirmation that this
 * doesn't silently overflow into whatever follows the caller's buffer. */
static void test_pat_create_packet_ts_rejects_too_many_programs_to_fit(void)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	for (int i = 0; i < 42; i++) {
		add_program(pat, i + 1, 0x100 + i);
	}
	CHECK(pat->program_count == 42);

	uint8_t pkt[188];
	CHECK(ltntstools_pat_create_packet_ts(pat, 0, pkt, 188) == 0); /* 185 bytes: fits exactly */

	add_program(pat, 43, 0x100 + 42); /* now 43 programs: 189 bytes, one too many */
	CHECK(pat->program_count == 43);

	uint8_t guarded[188 + 16];
	memset(guarded, 0xAA, sizeof(guarded));
	int ret = ltntstools_pat_create_packet_ts(pat, 0, guarded, 188);
	CHECK(ret < 0);

	/* The whole buffer, not just the trailing guard region, must be
	 * untouched -- the check must happen before anything is written. */
	uint8_t untouched[188 + 16];
	memset(untouched, 0xAA, sizeof(untouched));
	CHECK(memcmp(guarded, untouched, sizeof(guarded)) == 0);

	ltntstools_pat_free(pat);
}

static void test_create_packet_ts_rejects_invalid_args(void)
{
	uint8_t pkt[188];
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	CHECK(ltntstools_pat_create_packet_ts(NULL, 0, pkt, 188) < 0);
	CHECK(ltntstools_pat_create_packet_ts(pat, 0, NULL, 188) < 0);
	CHECK(ltntstools_pat_create_packet_ts(pat, 0, pkt, 100) < 0);
	ltntstools_pat_free(pat);

	struct ltntstools_pmt_s pmt = { 0 };
	CHECK(ltntstools_pmt_create_packet_ts(NULL, 0x50, 0, pkt, 188) < 0);
	CHECK(ltntstools_pmt_create_packet_ts(&pmt, 0x50, 0, NULL, 188) < 0);
	CHECK(ltntstools_pmt_create_packet_ts(&pmt, 0x50, 0, pkt, 100) < 0);
}

int main(void)
{
	test_alloc_free_basic();
	test_clone_null_returns_null();
	test_clone_matches_and_is_independent();
	test_dprintf_does_not_crash();

	test_pat_alloc_from_existing_truncates_and_logs_excess_programs();
	test_pat_alloc_from_existing_no_log_when_everything_fits();
	test_pat_add_from_existing_truncates_and_logs_excess_streams();
	test_pat_add_from_existing_logs_descriptor_add_failures();

	test_pmt_entry_compare();
	test_pmt_compare();
	test_pat_program_and_pat_compare();

	test_pmt_query_video_pid();
	test_pmt_entry_is_video();
	test_pmt_entry_is_audio_by_stream_type();
	test_pmt_entry_is_audio_by_private_data_descriptor();
	test_pmt_entry_is_scte35_outer_and_inner_descriptor();
	test_pmt_entry_is_smpte2038();

	test_get_services_teletext_found();
	test_get_services_teletext_none_found();
	test_get_services_teletext_null_args();

	test_enum_services_scte35_finds_and_terminates();
	test_enum_services_scte35_skips_zero_stream_program();
	test_enum_services_smpte2038_finds_and_terminates();
	test_enum_services_video_multi_program();
	test_enum_services_teletext_multi_program();
	test_enum_services_audio_multi_pid_single_program();
	test_enum_services_audio_skips_zero_stream_program();
	test_enum_services_by_pid();

	test_pmt_remove_es_for_pid_start_middle_end();
	test_pmt_remove_es_for_pid_not_found();

	test_pmt_enum_services_audio_array();
	test_pmt_enum_services_audio_array_zero_streams();

	test_pat_create_packet_ts_structure_and_crc();
	test_pat_create_packet_ts_version_number();
	test_pat_create_packet_ts_rejects_too_many_programs_to_fit();
	test_pmt_create_packet_ts_structure_and_crc();
	test_pmt_create_packet_ts_version_number();
	test_pmt_create_packet_ts_rejects_content_too_large_to_fit();
	test_pmt_create_packet_ts2_structure_and_crc();
	test_create_packet_ts_rejects_invalid_args();

	if (g_failures == 0) {
		printf("PASS: all pat tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d pat test(s) failed\n", g_failures);
	return 1;
}
