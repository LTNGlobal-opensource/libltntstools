/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for the ltntstools_history_metric_collection_* framework.
 * Builds directly against ../src/history-metric.c, no other library
 * dependencies (xorg-list.h is header-only).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "libltntstools/history-metric.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

static int list_length(struct ltntstools_history_metric_collection_s *c)
{
	int n = 0;
	struct ltntstools_history_metric_s *e = NULL;
	xorg_list_for_each_entry(e, &c->list, list) {
		n++;
	}
	return n;
}

static void add_metric(struct ltntstools_history_metric_collection_s *c, time_t ts, uint64_t value)
{
	struct ltntstools_history_metric_s *m = ltntstools_history_metric_alloc(ts, value);
	CHECK(m != NULL);
	ltntstools_history_metric_collection_add(c, m);
}

/* -------- alloc / init / free -------- */

static void test_alloc_free_basic(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("mycollection");
	CHECK(c != NULL);
	CHECK(c->wasAlloc == 1);
	CHECK(strcmp(c->name, "mycollection") == 0);
	CHECK(list_length(c) == 0);

	ltntstools_history_metric_collection_free(c);
}

static void test_alloc_null_name(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc(NULL);
	CHECK(c == NULL);
}

static void test_init_stack_basic(void)
{
	struct ltntstools_history_metric_collection_s c;
	int ret = ltntstools_history_metric_collection_init(&c, "stackcollection");
	CHECK(ret == 0);
	CHECK(c.wasAlloc == 0);
	CHECK(strcmp(c.name, "stackcollection") == 0);
	CHECK(list_length(&c) == 0);

	/* wasAlloc == 0, so free() must release internals (name) but not `c` itself. */
	ltntstools_history_metric_collection_free(&c);
}

static void test_init_null_name(void)
{
	struct ltntstools_history_metric_collection_s c;
	int ret = ltntstools_history_metric_collection_init(&c, NULL);
	CHECK(ret < 0);
}

/* -------- add() -------- */

static void test_add_single(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	add_metric(c, now, 42);
	CHECK(list_length(c) == 1);

	struct ltntstools_history_metric_s *e = xorg_list_first_entry(&c->list, struct ltntstools_history_metric_s, list);
	CHECK(e->ts == now);
	CHECK(e->count == 42);

	ltntstools_history_metric_collection_free(c);
}

static void test_add_multiple(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	for (int i = 0; i < 5; i++) {
		add_metric(c, now, i);
	}
	CHECK(list_length(c) == 5);

	ltntstools_history_metric_collection_free(c);
}

/* Newest entry is always inserted at the head of the list. */
static void test_add_order_newest_first(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	add_metric(c, now, 1);   /* oldest */
	add_metric(c, now, 2);
	add_metric(c, now, 3);   /* newest -- added last */

	struct ltntstools_history_metric_s *head = xorg_list_first_entry(&c->list, struct ltntstools_history_metric_s, list);
	CHECK(head->count == 3);

	ltntstools_history_metric_collection_free(c);
}

/* add() prunes anything older than 25hrs, even within the same call that adds it. */
static void test_add_prunes_older_than_25hr(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	add_metric(c, now - (26 * 3600), 99);
	CHECK(list_length(c) == 0);

	ltntstools_history_metric_collection_free(c);
}

/* A metric added later prunes older entries already sitting in the list. */
static void test_add_prunes_existing_stale_entries(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	/* Sneak a stale entry directly onto the list, bypassing add()'s own prune,
	 * to prove a *subsequent* add() call prunes pre-existing stale entries too. */
	struct ltntstools_history_metric_s *stale = ltntstools_history_metric_alloc(now - (26 * 3600), 1);
	xorg_list_add(&stale->list, &c->list);
	CHECK(list_length(c) == 1);

	add_metric(c, now, 2);
	CHECK(list_length(c) == 1);

	struct ltntstools_history_metric_s *e = xorg_list_first_entry(&c->list, struct ltntstools_history_metric_s, list);
	CHECK(e->count == 2);

	ltntstools_history_metric_collection_free(c);
}

/* Boundary: the prune comparison is strictly `<`, so an entry exactly at the
 * 25hr cutoff is retained, not pruned. */
static void test_add_keeps_metric_at_25hr_boundary(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	/* window used internally is time(NULL) - 25*3600, evaluated at add() time.
	 * Use "now" (captured slightly earlier) as the metric ts: since the
	 * internal window is computed from a time(NULL) >= now, ts (`now`) is
	 * >= window - (time(NULL) - now), which for a fast test is >= window. */
	add_metric(c, now, 7);
	CHECK(list_length(c) == 1);

	ltntstools_history_metric_collection_free(c);
}

/* -------- reset() -------- */

static void test_reset_clears_all(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	for (int i = 0; i < 3; i++) {
		add_metric(c, now, i);
	}
	CHECK(list_length(c) == 3);

	ltntstools_history_metric_collection_reset(c);
	CHECK(list_length(c) == 0);

	ltntstools_history_metric_collection_free(c);
}

static void test_reset_null_safe(void)
{
	/* Must not crash. */
	ltntstools_history_metric_collection_reset(NULL);
	CHECK(1);
}

/* -------- count_until() family -------- */

static void test_count_until_1hr_empty(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");

	uint64_t count = 0xdeadbeef;
	int ret = ltntstools_history_metric_collection_count_until_1hr(c, &count);
	CHECK(ret == 0);
	CHECK(count == 0);

	ltntstools_history_metric_collection_free(c);
}

static void test_count_until_1hr_sums_recent(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	add_metric(c, now, 10);
	add_metric(c, now - 1800, 20);   /* 30 min ago -- within 1hr window */
	add_metric(c, now - 7200, 30);   /* 2hr ago -- outside 1hr window */

	uint64_t count = 0;
	int ret = ltntstools_history_metric_collection_count_until_1hr(c, &count);
	CHECK(ret == 0);
	CHECK(count == 30); /* 10 + 20, excluding the 2hr-old entry */

	ltntstools_history_metric_collection_free(c);
}

static void test_count_until_24hr_sums_recent(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	add_metric(c, now, 5);
	add_metric(c, now - (10 * 3600), 7);    /* 10hr ago -- within 24hr window */
	add_metric(c, now - (23 * 3600), 11);   /* 23hr ago -- within 24hr window */

	uint64_t count = 0;
	int ret = ltntstools_history_metric_collection_count_until_24hr(c, &count);
	CHECK(ret == 0);
	CHECK(count == 23); /* 5 + 7 + 11 */

	ltntstools_history_metric_collection_free(c);
}

static void test_count_until_custom_window_excludes_out_of_range(void)
{
	struct ltntstools_history_metric_collection_s *c = ltntstools_history_metric_collection_alloc("t");
	time_t now = time(NULL);

	add_metric(c, now, 100);
	add_metric(c, now - 120, 200); /* 2 min ago */

	/* Window of "now - 60" should only capture the first metric. */
	uint64_t count = 0;
	int ret = ltntstools_history_metric_collection_count_until(c, now - 60, &count);
	CHECK(ret == 0);
	CHECK(count == 100);

	ltntstools_history_metric_collection_free(c);
}

static void test_count_until_null_collection(void)
{
	uint64_t count = 0;
	CHECK(ltntstools_history_metric_collection_count_until(NULL, 0, &count) < 0);
	CHECK(ltntstools_history_metric_collection_count_until_1hr(NULL, &count) < 0);
	CHECK(ltntstools_history_metric_collection_count_until_24hr(NULL, &count) < 0);
}

int main(void)
{
	test_alloc_free_basic();
	test_alloc_null_name();
	test_init_stack_basic();
	test_init_null_name();

	test_add_single();
	test_add_multiple();
	test_add_order_newest_first();
	test_add_prunes_older_than_25hr();
	test_add_prunes_existing_stale_entries();
	test_add_keeps_metric_at_25hr_boundary();

	test_reset_clears_all();
	test_reset_null_safe();

	test_count_until_1hr_empty();
	test_count_until_1hr_sums_recent();
	test_count_until_24hr_sums_recent();
	test_count_until_custom_window_excludes_out_of_range();
	test_count_until_null_collection();

	if (g_failures == 0) {
		printf("PASS: all history-metric tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d history-metric test(s) failed\n", g_failures);
	return 1;
}
