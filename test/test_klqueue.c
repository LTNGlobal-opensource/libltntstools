/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/kl-queue.c / src/libltntstools/kl-queue.h.
 * Builds against ../src/kl-queue.c only (xorg-list.h is header-only).
 * Requires -lpthread.
 *
 * struct klqueue_s is fully public (not opaque), so a couple of tests
 * below read q.item_count / q.head directly rather than going through
 * klqueue_count() -- see the note on klqueue_destroy() leaving the mutex
 * locked, below.
 *
 * KNOWN BUG PINNED HERE (not fixed, just documented so it's a deliberate
 * decision if it ever changes): klqueue_pop_non_blocking()'s timeout is
 * broken. pthread_cond_timedwait() requires an *absolute* deadline
 * (CLOCK_REALTIME, since the Epoch), but the implementation builds it as
 *     struct timespec abstime = { usec / 1000000, usec % 1000000 };
 * ie. treats the caller's *relative* microsecond timeout as if it were
 * already an absolute time near the Epoch (and even mixes up units doing
 * it -- the second field of timespec is nanoseconds, not microseconds).
 * The resulting deadline is always far in the past, so whenever the queue
 * is empty at call time, pthread_cond_timedwait() returns ETIMEDOUT
 * essentially instantly -- the requested timeout is never actually
 * honoured, and a push that lands shortly after the call (well within the
 * requested window) is missed. See
 * test_pop_non_blocking_empty_queue_times_out_immediately() and
 * test_pop_non_blocking_does_not_wait_for_a_racing_push().
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include "libltntstools/kl-queue.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

#define CHECK_EQ_U64(a, b) \
	do { \
		uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); \
		if (_a != _b) { \
			fprintf(stderr, "FAIL: %s:%d: %s (%" PRIu64 ") != %s (%" PRIu64 ")\n", \
				__FILE__, __LINE__, #a, _a, #b, _b); \
			g_failures++; \
		} \
	} while (0)

static double elapsed_ms(const struct timespec *t0, const struct timespec *t1)
{
	return (t1->tv_sec - t0->tv_sec) * 1000.0 + (t1->tv_nsec - t0->tv_nsec) / 1e6;
}

/* ---------------------------------------------------------------------
 * Basic lifecycle: initialize / count / empty / push
 * --------------------------------------------------------------------- */

static void test_initialize_starts_empty(void)
{
	struct klqueue_s q;
	klqueue_initialize(&q);

	CHECK_EQ_U64(klqueue_count(&q), 0);
	CHECK(klqueue_empty(&q) == 1);

	klqueue_destroy(&q);
}

static void test_push_increments_count_and_clears_empty(void)
{
	struct klqueue_s q;
	klqueue_initialize(&q);

	int a = 1, b = 2, c = 3;

	klqueue_push(&q, &a);
	CHECK_EQ_U64(klqueue_count(&q), 1);
	CHECK(klqueue_empty(&q) == 0);

	klqueue_push(&q, &b);
	klqueue_push(&q, &c);
	CHECK_EQ_U64(klqueue_count(&q), 3);

	/* Drain before destroy() so it doesn't print its (expected, but
	 * irrelevant to this test) leaked-pointer warning. */
	void *item;
	while (klqueue_pop_non_blocking(&q, 0, &item) == 0) { }
	klqueue_destroy(&q);
}

/* ---------------------------------------------------------------------
 * FIFO ordering and count bookkeeping across pop_non_blocking()
 * --------------------------------------------------------------------- */

static void test_pop_non_blocking_fifo_order_and_data(void)
{
	struct klqueue_s q;
	klqueue_initialize(&q);

	int a = 11, b = 22, c = 33;
	klqueue_push(&q, &a);
	klqueue_push(&q, &b);
	klqueue_push(&q, &c);
	CHECK_EQ_U64(klqueue_count(&q), 3);

	/* The queue already has items, so pop_non_blocking() takes the
	 * "already available" fast path (ret == 0 without waiting), regardless
	 * of the requested timeout -- a generous value is used here only to
	 * make that explicit. */
	void *item = NULL;

	CHECK(klqueue_pop_non_blocking(&q, 100000, &item) == 0);
	CHECK(item == &a); /* first pushed, first popped */
	CHECK_EQ_U64(klqueue_count(&q), 2);

	CHECK(klqueue_pop_non_blocking(&q, 100000, &item) == 0);
	CHECK(item == &b);
	CHECK_EQ_U64(klqueue_count(&q), 1);

	CHECK(klqueue_pop_non_blocking(&q, 100000, &item) == 0);
	CHECK(item == &c);
	CHECK_EQ_U64(klqueue_count(&q), 0);
	CHECK(klqueue_empty(&q) == 1);

	klqueue_destroy(&q);
}

/* ---------------------------------------------------------------------
 * pop_non_blocking() on an empty queue -- see the KNOWN BUG note above.
 * --------------------------------------------------------------------- */

static void test_pop_non_blocking_empty_queue_times_out_immediately(void)
{
	struct klqueue_s q;
	klqueue_initialize(&q);

	void *item = (void *)0xdeadbeef; /* sentinel, must be left untouched on timeout */

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int ret = klqueue_pop_non_blocking(&q, 300000 /* 300ms requested */, &item);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	CHECK(ret == ETIMEDOUT);
	CHECK(item == (void *)0xdeadbeef); /* untouched */
	/* The requested 300ms is never actually honoured -- this returns in a
	 * fraction of a millisecond. A generous 50ms bound comfortably
	 * distinguishes "returned immediately" from "actually waited". */
	CHECK(elapsed_ms(&t0, &t1) < 50.0);

	klqueue_destroy(&q);
}

struct pusher_arg_s {
	struct klqueue_s *q;
	int delay_ms;
	int value;
};

static void *pusher_thread(void *arg)
{
	struct pusher_arg_s *pa = arg;
	usleep(pa->delay_ms * 1000);
	klqueue_push(pa->q, &pa->value);
	return NULL;
}

/* Demonstrates the practical consequence of the timeout bug: a push that
 * lands comfortably inside the requested wait window is still missed,
 * because pop_non_blocking() never actually blocks when the queue starts
 * empty. */
static void test_pop_non_blocking_does_not_wait_for_a_racing_push(void)
{
	struct klqueue_s q;
	klqueue_initialize(&q);

	struct pusher_arg_s pa = { .q = &q, .delay_ms = 50, .value = 77 };
	pthread_t th;
	pthread_create(&th, NULL, pusher_thread, &pa);

	void *item = NULL;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	/* Requested timeout (400ms) comfortably covers the push at +50ms. */
	int ret = klqueue_pop_non_blocking(&q, 400000, &item);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	CHECK(ret == ETIMEDOUT); /* the item is missed, not delivered */
	CHECK(elapsed_ms(&t0, &t1) < 50.0);

	pthread_join(th, NULL);

	/* The push did land -- it's just sitting on the queue now, available
	 * to a subsequent call. */
	CHECK_EQ_U64(klqueue_count(&q), 1);
	CHECK(klqueue_pop_non_blocking(&q, 100000, &item) == 0);
	CHECK(item == &pa.value);

	klqueue_destroy(&q);
}

/* ---------------------------------------------------------------------
 * destroy() drains the list. Note: per the implementation's own comment
 * it "intentionally leaves the queue locked", so callers must not touch
 * the queue (not even klqueue_count(), which locks the mutex) after
 * destroy() without re-initializing it first. To check post-destroy
 * state without deadlocking, this test reads the public struct fields
 * directly instead of calling the locking accessors.
 * --------------------------------------------------------------------- */

static void test_destroy_drains_unpopped_items(void)
{
	struct klqueue_s q;
	klqueue_initialize(&q);

	int a = 1, b = 2;
	klqueue_push(&q, &a);
	klqueue_push(&q, &b);
	CHECK_EQ_U64(klqueue_count(&q), 2);

	/* destroy() prints an expected "leaking N user pointers" warning to
	 * stderr here -- that's normal for this test, it's deliberately
	 * exercising the drain-on-teardown path with un-popped items left behind. */
	klqueue_destroy(&q);

	CHECK_EQ_U64(q.item_count, 0);
	CHECK(xorg_list_is_empty(&q.head) == 1);
}

int main(void)
{
	test_initialize_starts_empty();
	test_push_increments_count_and_clears_empty();
	test_pop_non_blocking_fifo_order_and_data();
	test_pop_non_blocking_empty_queue_times_out_immediately();
	test_pop_non_blocking_does_not_wait_for_a_racing_push();
	test_destroy_drains_unpopped_items();

	if (g_failures == 0) {
		printf("PASS: all kl-queue tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d kl-queue test(s) failed\n", g_failures);
	return 1;
}
