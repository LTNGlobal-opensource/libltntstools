/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/streammodel-extractors.c (extractors_alloc/add/write/free),
 * the TR101290 P2.2 section-CRC-check machinery used internally by
 * src/streammodel.c. These functions aren't part of the public API (no
 * libltntstools/streammodel-extractors.h) -- they're declared in the
 * library-private src/streammodel-types.h, which this test includes
 * directly, exactly as src/streammodel-extractors.c itself does.
 *
 * Builds against ../src/streammodel-extractors.c plus its
 * ../src/sectionextractor.c, ../src/ts.c and ../src/crc32.c dependencies.
 * streammodel-types.h pulls in <dvbpsi/...> headers for type declarations
 * (struct streammodel_pid_s embeds dvbpsi_pat_t and dvbpsi_pmt_t pointers),
 * so DVBPSI_CFLAGS is needed to compile, but nothing here calls into
 * libdvbpsi, so -ldvbpsi isn't needed to link.
 *
 * struct streammodel_ctx_s embeds two full struct streammodel_rom_s models
 * (roms[2]), each with a pids[MAX_ROM_PIDS] array (MAX_ROM_PIDS == 0x2000)
 * of struct streammodel_pid_s -- around 10MB total. The real code (see
 * ltntstools_streammodel_alloc() in streammodel.c) always calloc()s this on
 * the heap; declaring one as a plain local blows the default ~8MB thread
 * stack immediately on function entry. These tests do the same: ctx_new()
 * heap-allocates it, matching production usage.
 *
 * SCOPE: these tests drive extractors_alloc/add/write/free directly against
 * a struct streammodel_ctx_s, without going through
 * ltntstools_streammodel_alloc()/_write() (which would additionally require
 * linking pat.c/descriptor.c/stats.c/clocks.c/history-metric.c and
 * libdvbpsi -- see test_streammodel.c for that full integration path).
 * ctx->rom_mutex is never touched here since these functions don't lock it.
 *
 * extractors_write()'s pid dispatch (case 0x00/0x10/0x11/0x12/0x14, default)
 * indexes directly into ctx->seArray by hardcoded position, matched to the
 * exact call order of the 9 extractors_add() calls in extractors_alloc().
 * The comment in that source file ("don't add anything between here...
 * without adjusting the _write switch table hard-coded indexes") flags
 * this as a fragile coupling -- test_write_dispatches_each_fixed_pid_to_the_
 * matching_table_id below pins down that every one of those 9 fixed slots
 * routes to the extractor with the matching table_id (and only that one),
 * which is exactly the invariant a future append-without-adjusting-the-
 * switch mistake would break.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "streammodel-types.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* Builds a single TS packet containing a complete, standalone section:
 * PUSI=1, pointer_field=0, given table_id, and (unless corruptCRC) a valid
 * trailing CRC32. Mirrors test_sectionextractor.c's build_section_packet(). */
static void build_section_packet(uint8_t *pkt, uint16_t pid, uint8_t tableId, uint8_t cc,
	const uint8_t *payload, int payloadLen, int corruptCRC)
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
	if (corruptCRC)
		crc ^= 0xffffffff;
	pkt[8 + payloadLen + 0] = (crc >> 24) & 0xff;
	pkt[8 + payloadLen + 1] = (crc >> 16) & 0xff;
	pkt[8 + payloadLen + 2] = (crc >> 8) & 0xff;
	pkt[8 + payloadLen + 3] = crc & 0xff;
}

static uint8_t g_payload[6] = { 'A', 'A', 'A', 'A', 'A', 'A' };

/* -------- callback capture -------- */

static int g_cbCount;
static struct streammodel_callback_args_s g_lastArgs;
static void *g_lastUserContext;

static void reset_cb_capture(void)
{
	g_cbCount = 0;
	memset(&g_lastArgs, 0, sizeof(g_lastArgs));
	g_lastUserContext = NULL;
}

static void test_cb(void *userContext, struct streammodel_callback_args_s *args)
{
	g_cbCount++;
	g_lastArgs = *args;
	g_lastUserContext = userContext;
}

static struct streammodel_ctx_s *ctx_new(int enable)
{
	struct streammodel_ctx_s *ctx = calloc(1, sizeof(*ctx));
	ctx->enableSectionCRCChecks = enable;
	ctx->cb = test_cb;
	ctx->userContext = (void *)0x1234;
	return ctx;
}

/* -------- extractors_alloc -------- */

static void test_alloc_is_noop_when_checks_disabled(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(0);

	CHECK(extractors_alloc(ctx) == 0);
	CHECK(ctx->seCount == 0);
	CHECK(ctx->seArray == NULL);

	free(ctx);
}

static void test_alloc_populates_nine_fixed_extractors(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);

	CHECK(extractors_alloc(ctx) == 0);
	CHECK(ctx->seCount == 9);
	CHECK(ctx->seArray != NULL);

	static const struct { uint16_t pid; uint8_t tableId; uint32_t context; const char *name; } expect[9] = {
		{ 0x00, 0x00, STREAMMODEL_CB_CONTEXT_PAT, "PAT" },
		{ 0x10, 0x40, STREAMMODEL_CB_CONTEXT_NIT, "NIT" },
		{ 0x11, 0x4A, STREAMMODEL_CB_CONTEXT_BAT, "BAT" },
		{ 0x11, 0x42, STREAMMODEL_CB_CONTEXT_SDT, "SDT" },
		{ 0x12, 0x4E, STREAMMODEL_CB_CONTEXT_EIT, "EIT" },
		{ 0x12, 0x4F, STREAMMODEL_CB_CONTEXT_EIT, "EIT" },
		{ 0x12, 0x5F, STREAMMODEL_CB_CONTEXT_EIT, "EIT" },
		{ 0x12, 0x6F, STREAMMODEL_CB_CONTEXT_EIT, "EIT" },
		{ 0x14, 0x73, STREAMMODEL_CB_CONTEXT_TOT, "TOT" },
	};

	for (int i = 0; i < 9; i++) {
		struct se_array_item_s *item = &ctx->seArray[i];
		CHECK(item->pid == expect[i].pid);
		CHECK(item->tableId == expect[i].tableId);
		CHECK(item->context == expect[i].context);
		CHECK(item->name != NULL && strcmp(item->name, expect[i].name) == 0);
		CHECK(item->hdl != NULL);
	}

	extractors_free(ctx);
	free(ctx);
}

/* -------- extractors_free -------- */

static void test_free_disabled_ctx_is_safe_noop(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(0);

	extractors_free(ctx); /* must not crash with seArray == NULL */
	CHECK(1);

	free(ctx);
}

static void test_free_releases_array(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);
	extractors_alloc(ctx);
	CHECK(ctx->seArray != NULL);

	extractors_free(ctx);
	CHECK(ctx->seArray == NULL);

	free(ctx);
}

/* -------- extractors_add -------- */

static void test_add_appends_new_entry(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);

	CHECK(extractors_add(ctx, 0x123, 0xFC, "TEST", 42) == 0);
	CHECK(ctx->seCount == 1);
	CHECK(ctx->seArray[0].pid == 0x123);
	CHECK(ctx->seArray[0].tableId == 0xFC);
	CHECK(ctx->seArray[0].context == 42);
	CHECK(strcmp(ctx->seArray[0].name, "TEST") == 0);
	CHECK(ctx->seArray[0].hdl != NULL);

	extractors_free(ctx);
	free(ctx);
}

static void test_add_pmt_dedups_by_pid_after_fixed_slots(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);
	extractors_alloc(ctx); /* seeds the 9 fixed slots (indices 0-8) */

	CHECK(extractors_add(ctx, 0x200, 0x02 /* PMT */, "PMT", STREAMMODEL_CB_CONTEXT_PMT) == 0);
	CHECK(ctx->seCount == 10);

	/* Re-adding the same PMT pid must be recognized as already-present and
	 * not grow the array again. */
	CHECK(extractors_add(ctx, 0x200, 0x02, "PMT", STREAMMODEL_CB_CONTEXT_PMT) == 0);
	CHECK(ctx->seCount == 10);

	/* A different PMT pid is a genuinely new entry. */
	CHECK(extractors_add(ctx, 0x300, 0x02, "PMT", STREAMMODEL_CB_CONTEXT_PMT) == 0);
	CHECK(ctx->seCount == 11);

	extractors_free(ctx);
	free(ctx);
}

/* -------- extractors_write -------- */

static void test_write_returns_packet_count(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);
	extractors_alloc(ctx);

	uint8_t pkts[2][188];
	build_section_packet(pkts[0], 0x1FFE /* no matching extractor */, 0xFC, 0, g_payload, sizeof(g_payload), 0);
	build_section_packet(pkts[1], 0x1FFE, 0xFC, 0, g_payload, sizeof(g_payload), 0);

	CHECK(extractors_write(ctx, &pkts[0][0], 2) == 2);

	extractors_free(ctx);
	free(ctx);
}

static void test_write_unmatched_pid_does_not_crash_or_callback(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);
	extractors_alloc(ctx);
	reset_cb_capture();

	uint8_t pkt[188];
	build_section_packet(pkt, 0x1FFE, 0xFC, 0, g_payload, sizeof(g_payload), 0);
	extractors_write(ctx, pkt, 1);

	CHECK(g_cbCount == 0);

	extractors_free(ctx);
	free(ctx);
}

static void test_write_dispatches_each_fixed_pid_to_the_matching_table_id(void)
{
	/* For every one of the 9 fixed slots, a section on that slot's pid and
	 * table_id must complete only that slot (crcValid) and fire exactly
	 * one callback with that slot's context -- pinning down the
	 * hardcoded-index coupling between extractors_alloc() and
	 * extractors_write()'s switch table. */
	static const struct { uint16_t pid; uint8_t tableId; uint32_t context; int slot; } cases[9] = {
		{ 0x00, 0x00, STREAMMODEL_CB_CONTEXT_PAT, 0 },
		{ 0x10, 0x40, STREAMMODEL_CB_CONTEXT_NIT, 1 },
		{ 0x11, 0x4A, STREAMMODEL_CB_CONTEXT_BAT, 2 },
		{ 0x11, 0x42, STREAMMODEL_CB_CONTEXT_SDT, 3 },
		{ 0x12, 0x4E, STREAMMODEL_CB_CONTEXT_EIT, 4 },
		{ 0x12, 0x4F, STREAMMODEL_CB_CONTEXT_EIT, 5 },
		{ 0x12, 0x5F, STREAMMODEL_CB_CONTEXT_EIT, 6 },
		{ 0x12, 0x6F, STREAMMODEL_CB_CONTEXT_EIT, 7 },
		{ 0x14, 0x73, STREAMMODEL_CB_CONTEXT_TOT, 8 },
	};

	for (int c = 0; c < 9; c++) {
		struct streammodel_ctx_s *ctx = ctx_new(1);
		extractors_alloc(ctx);
		reset_cb_capture();

		uint8_t pkt[188];
		build_section_packet(pkt, cases[c].pid, cases[c].tableId, 0, g_payload, sizeof(g_payload), 0);
		extractors_write(ctx, pkt, 1);

		CHECK(ctx->seArray[cases[c].slot].complete == 1);
		CHECK(ctx->seArray[cases[c].slot].crcValid == 1);
		CHECK(g_cbCount == 1);
		CHECK(g_lastArgs.status == STREAMMODEL_CB_CRC_STATUS);
		CHECK(g_lastArgs.context == cases[c].context);
		CHECK(g_lastArgs.arg == CRC_ARG_VALID);
		CHECK(g_lastUserContext == ctx->userContext);

		/* Every other fixed slot sharing the same pid must NOT have
		 * completed -- the table_id filter, not just the pid switch,
		 * is what selects the right extractor (pid 0x11 -> BAT+SDT,
		 * pid 0x12 -> four EIT slots). */
		for (int other = 0; other < 9; other++) {
			if (other == cases[c].slot)
				continue;
			if (ctx->seArray[other].pid != cases[c].pid)
				continue;
			CHECK(ctx->seArray[other].complete == 0);
		}

		extractors_free(ctx);
		free(ctx);
	}
}

static void test_write_invalid_crc_still_completes_but_marks_invalid(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);
	extractors_alloc(ctx);
	reset_cb_capture();

	uint8_t pkt[188];
	build_section_packet(pkt, 0x00, 0x00, 0, g_payload, sizeof(g_payload), 1 /* corrupt CRC */);
	extractors_write(ctx, pkt, 1);

	CHECK(ctx->seArray[0].complete == 1);
	CHECK(ctx->seArray[0].crcValid == 0);
	CHECK(g_cbCount == 1);
	CHECK(g_lastArgs.arg == CRC_ARG_INVALID);
	CHECK(g_lastArgs.context == STREAMMODEL_CB_CONTEXT_PAT);

	extractors_free(ctx);
	free(ctx);
}

static void test_write_default_case_dispatches_to_dynamically_added_extractor(void)
{
	struct streammodel_ctx_s *ctx = ctx_new(1);
	extractors_alloc(ctx); /* slots 0-8 */
	extractors_add(ctx, 0x300, 0x02 /* PMT */, "PMT", STREAMMODEL_CB_CONTEXT_PMT); /* slot 9 */
	reset_cb_capture();

	uint8_t pkt[188];
	build_section_packet(pkt, 0x300, 0x02, 0, g_payload, sizeof(g_payload), 0);
	extractors_write(ctx, pkt, 1);

	CHECK(ctx->seArray[9].complete == 1);
	CHECK(ctx->seArray[9].crcValid == 1);
	CHECK(g_cbCount == 1);
	CHECK(g_lastArgs.context == STREAMMODEL_CB_CONTEXT_PMT);

	extractors_free(ctx);
	free(ctx);
}

int main(void)
{
	test_alloc_is_noop_when_checks_disabled();
	test_alloc_populates_nine_fixed_extractors();

	test_free_disabled_ctx_is_safe_noop();
	test_free_releases_array();

	test_add_appends_new_entry();
	test_add_pmt_dedups_by_pid_after_fixed_slots();

	test_write_returns_packet_count();
	test_write_unmatched_pid_does_not_crash_or_callback();
	test_write_dispatches_each_fixed_pid_to_the_matching_table_id();
	test_write_invalid_crc_still_completes_but_marks_invalid();
	test_write_default_case_dispatches_to_dynamically_added_extractor();

	if (g_failures == 0) {
		printf("PASS: all streammodel-extractors tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d streammodel-extractors test(s) failed\n", g_failures);
	return 1;
}
