/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/proc-net-udp.c / src/libltntstools/proc-net-udp.h.
 * Builds against ../src/proc-net-udp.c directly (self-contained, no other
 * src source-file dependencies). Requires -lpthread since
 * ltntstools_proc_net_udp_alloc() spawns a background polling thread.
 *
 * SCOPE NOTE: this module's actual data source is Linux's /proc/net/udp
 * and /proc/<pid>/fd entries, which don't exist on this (non-Linux)
 * build/test machine -- fopen("/proc/net/udp") and opendir("/proc") simply fail here,
 * so the background thread's _tableBuilderSockets()/_tableBuilderProcesses()
 * never populate ctx->items. That means the "socket table got built from
 * /proc" path can't be exercised from this test file; instead this
 * exercises the OS-independent logic: the pure array helper
 * (ltntstools_proc_net_udp_find_inode), the dprintf formatter (fed a
 * manually constructed item array, since it never touches /proc itself),
 * and the alloc/free lifecycle (including a rapid-cycle stress loop, since
 * an immediate alloc-then-free used to race with the background thread's
 * startup and free memory the thread was still about to touch -- see the
 * ctx->threadRunning fix in ltntstools_proc_net_udp_alloc()).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/proc-net-udp.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- ltntstools_proc_net_udp_find_inode() -------- */

static void makeItems(struct ltntstools_proc_net_udp_item_s *items, int count)
{
	memset(items, 0, count * sizeof(*items));
	for (int i = 0; i < count; i++) {
		items[i].inode = 1000 + i;
	}
}

static void test_find_inode_null_array(void)
{
	CHECK(ltntstools_proc_net_udp_find_inode(NULL, 0, 1000) == NULL);
	CHECK(ltntstools_proc_net_udp_find_inode(NULL, 5, 1000) == NULL);
}

static void test_find_inode_zero_or_negative_count(void)
{
	struct ltntstools_proc_net_udp_item_s items[3];
	makeItems(items, 3);

	CHECK(ltntstools_proc_net_udp_find_inode(items, 0, 1000) == NULL);
	CHECK(ltntstools_proc_net_udp_find_inode(items, -1, 1000) == NULL);
}

static void test_find_inode_match_and_no_match(void)
{
	struct ltntstools_proc_net_udp_item_s items[3];
	makeItems(items, 3);

	struct ltntstools_proc_net_udp_item_s *found = ltntstools_proc_net_udp_find_inode(items, 3, 1001);
	CHECK(found == &items[1]);
	CHECK(found->inode == 1001);

	CHECK(ltntstools_proc_net_udp_find_inode(items, 3, 9999) == NULL);
}

/* -------- ltntstools_proc_net_udp_item_free() -------- */

static void test_item_free_null_safe(void)
{
	ltntstools_proc_net_udp_item_free(NULL, NULL);
	CHECK(1); /* must not crash */
}

static void test_item_free_allocated_array(void)
{
	struct ltntstools_proc_net_udp_item_s *items = malloc(2 * sizeof(*items));
	CHECK(items != NULL);
	ltntstools_proc_net_udp_item_free(NULL, items);
	CHECK(1); /* must not crash / double count */
}

/* -------- ltntstools_proc_net_udp_item_dprintf() -------- */

static void test_dprintf_format(void)
{
	struct ltntstools_proc_net_udp_item_s items[1];
	memset(items, 0, sizeof(items));

	items[0].sl = 5;
	strcpy(items[0].locaddr, "0.0.0.0:5000");
	strcpy(items[0].remaddr, "0.0.0.0:0");
	items[0].drops = 42;
	items[0].uid = 1000;
	items[0].inode = 123456;
	items[0].pidListCount = 2;
	items[0].pidList[0].pid = 111;
	strcpy((char *)items[0].pidList[0].comm, "proc1");
	items[0].pidList[1].pid = 222;
	strcpy((char *)items[0].pidList[1].comm, "proc2");

	FILE *tmp = tmpfile();
	CHECK(tmp != NULL);
	if (!tmp) {
		return;
	}

	ltntstools_proc_net_udp_item_dprintf(NULL, fileno(tmp), items, 1);
	fseek(tmp, 0, SEEK_SET);

	char header[512] = { 0 };
	char row[512] = { 0 };
	CHECK(fgets(header, sizeof(header), tmp) != NULL);
	CHECK(fgets(row, sizeof(row), tmp) != NULL);

	CHECK(strstr(header, "sl") != NULL);
	CHECK(strstr(header, "loc") != NULL);
	CHECK(strstr(header, "rem") != NULL);
	CHECK(strstr(header, "drops") != NULL);
	CHECK(strstr(header, "uid") != NULL);
	CHECK(strstr(header, "inode") != NULL);
	CHECK(strstr(header, "process") != NULL);

	CHECK(strstr(row, "0.0.0.0:5000") != NULL);
	CHECK(strstr(row, "0.0.0.0:0") != NULL);
	CHECK(strstr(row, "42") != NULL);
	CHECK(strstr(row, "123456") != NULL);
	CHECK(strstr(row, "111 proc1") != NULL);
	CHECK(strstr(row, ", 222 proc2") != NULL);

	fclose(tmp);
}

static void test_dprintf_zero_items_no_crash(void)
{
	FILE *tmp = tmpfile();
	CHECK(tmp != NULL);
	if (!tmp) {
		return;
	}

	ltntstools_proc_net_udp_item_dprintf(NULL, fileno(tmp), NULL, 0);
	fseek(tmp, 0, SEEK_SET);

	char header[512] = { 0 };
	CHECK(fgets(header, sizeof(header), tmp) != NULL);
	CHECK(strstr(header, "sl") != NULL); /* header always printed, even with no rows */

	fclose(tmp);
}

/* -------- ltntstools_proc_net_udp_alloc() / free() lifecycle -------- */

static void test_alloc_free_basic(void)
{
	void *hdl = NULL;
	CHECK(ltntstools_proc_net_udp_alloc(&hdl) == 0);
	CHECK(hdl != NULL);
	ltntstools_proc_net_udp_free(hdl);
}

static void test_alloc_free_rapid_cycles_no_uaf(void)
{
	/* Regression coverage: an immediate alloc()-then-free() used to race
	 * with the background thread's startup, freeing ctx before the thread
	 * had set ctx->threadRunning, so free() skipped the terminate/wait
	 * handshake and the detached thread went on to touch freed memory.
	 * Run several back-to-back cycles with no delay to stress that path. */
	for (int i = 0; i < 25; i++) {
		void *hdl = NULL;
		CHECK(ltntstools_proc_net_udp_alloc(&hdl) == 0);
		CHECK(hdl != NULL);
		ltntstools_proc_net_udp_free(hdl);
	}
}

static void test_query_before_any_items_fails_cleanly(void)
{
	/* No /proc/net/udp on this platform, so the background thread never
	 * populates any items -- query is expected to report failure rather
	 * than fabricate an empty-but-successful result. */
	void *hdl = NULL;
	ltntstools_proc_net_udp_alloc(&hdl);

	struct ltntstools_proc_net_udp_item_s *array = NULL;
	int count = -1;
	CHECK(ltntstools_proc_net_udp_item_query(hdl, &array, &count) < 0);

	ltntstools_proc_net_udp_free(hdl);
}

static void test_reset_drops_on_empty_table_no_crash(void)
{
	void *hdl = NULL;
	ltntstools_proc_net_udp_alloc(&hdl);

	ltntstools_proc_net_udp_items_reset_drops(hdl);
	CHECK(1); /* must not crash on an empty (itemCount == 0) table */

	ltntstools_proc_net_udp_free(hdl);
}

int main(void)
{
	test_find_inode_null_array();
	test_find_inode_zero_or_negative_count();
	test_find_inode_match_and_no_match();

	test_item_free_null_safe();
	test_item_free_allocated_array();

	test_dprintf_format();
	test_dprintf_zero_items_no_crash();

	test_alloc_free_basic();
	test_alloc_free_rapid_cycles_no_uaf();
	test_query_before_any_items_fails_cleanly();
	test_reset_drops_on_empty_table_no_crash();

	if (g_failures == 0) {
		printf("PASS: all proc-net-udp tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d proc-net-udp test(s) failed\n", g_failures);
	return 1;
}
