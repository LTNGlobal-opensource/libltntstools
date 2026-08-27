/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/tr101290.c, src/tr101290-events.c, src/tr101290-alarms.c,
 * src/tr101290-timers.c, src/tr101290-p1.c, src/tr101290-p2.c and
 * src/tr101290-summary.c / src/libltntstools/tr101290.h.
 *
 * Builds against those seven .c files plus their transitive dependencies:
 * streammodel.c, streammodel-extractors.c, pat.c (also used directly here
 * to build a synthetic PAT packet), descriptor.c, crc32.c, ts.c,
 * sectionextractor.c, stats.c, clocks.c, history-metric.c. Links -lpthread
 * and (on Linux) -lrt for the POSIX timer calls in tr101290-timers.c.
 *
 * SCOPE / DESIGN NOTE: this module includes its own library-private
 * tr101290-types.h, which this test includes directly (as every
 * tr101290-*.c file itself does) to reach struct ltntstools_tr101290_s and
 * the internal (non-static, but undocumented/unexported) helper functions
 * declared in tr101290-events.h/-alarms.h/-timers.h/-p1.h/-p2.h --
 * ltntstools_tr101290_alarm_raise(), _tr101290_event_clear(),
 * ltntstools_tr101290_event_should_report(), p2_process_pat_model(), etc.
 * This is deliberate: it lets the alarm-raise/clear/debounce state machine
 * (this module's real complexity) be tested directly and fast, without
 * needing to wait on the ~50ms background thread poll cycle to observe
 * state via the public callback. Two tiers are used:
 *   - "light" tests build a bare struct ltntstools_tr101290_s (just
 *     event_tbl, via ltntstools_tr101290_event_table_copy()) and call the
 *     pure event/alarm helpers directly -- none of those touch
 *     streamStatistics/smHandle/the mutexes, so this is safe and fast.
 *   - "public API" tests go through the real ltntstools_tr101290_alloc()/
 *     _write()/_free() (which _does_ spin up the real background thread,
 *     streammodel, and pid-stats subsystems), but even then, alarm state
 *     changes are observed synchronously via
 *     ltntstools_tr101290_summary_get() (which reads live event_tbl state
 *     directly) rather than by polling the async notification callback --
 *     avoiding thread-timing-dependent flakiness entirely.
 *
 * PER THE TASK INSTRUCTIONS, NO CHANGES WERE MADE to any tr101290-*.c or
 * tr101290*.h file, even though testing surfaced several real, worth-fixing
 * issues. Documented here (not fixed):
 *
 * 1. (Same class already found and fixed elsewhere this session, in
 *    smoother-pcr.c/smoother-rtp.c/probes.c/segmentwriter.c) A use-after-free
 *    race: ltntstools_tr101290_alloc() spawns its background thread via a
 *    bare `pthread_create(...)` (return value discarded) as its last step,
 *    and ltntstools_tr101290_threadFunc() sets s->threadRunning = 1 itself
 *    once it starts running -- not the parent, synchronously, before/at
 *    spawn time. ltntstools_tr101290_free() gates its "wait for the thread
 *    to terminate" logic on that flag, so free() called before the new
 *    thread is first scheduled could skip the wait and let the thread
 *    dereference an already-freed context. This module's alloc() does
 *    substantially more synchronous setup work before spawning the thread
 *    than the other affected modules did (event table clone, streammodel
 *    alloc, timer creation loop), which narrows the real-world race window
 *    but does not close it. Every test below that calls
 *    ltntstools_tr101290_free() does so only after some further work (a
 *    write() call, or a short sleep) rather than immediately after alloc(),
 *    to avoid exercising this known-but-unfixable-here race.
 *
 * 2. ltntstools_tr101290_event_should_report() (tr101290-events.c) computes
 *    `nextReport = lastReported + ev->reportInterval` via timeradd(), then
 *    never references `nextReport` again -- the actual gating decision only
 *    compares `lastReported >= lastChanged`. reportInterval is always
 *    {0,0} in the static tr_events_tbl default table regardless, so this
 *    looks like a partially-implemented "throttle repeated reports of a
 *    still-raised alarm to once per reportInterval" feature that was never
 *    wired up. Actual (tested) behavior: an event is reported exactly once
 *    per raised/cleared *state transition*, not on any periodic interval.
 *
 * 3. p2_process_p2_2() (tr101290-p2.c), which is meant to implement the P2.2
 *    "CRC error occurred in CAT/PAT/PMT/NIT/EIT/BAT/SDT/TOT table" check via
 *    a table-arrival-staleness window, unconditionally `return`s as its
 *    first statement ("Not specific check to ensure these are delivered on
 *    time in p2.2"), before ever touching s->p2.lastPAT/PMT/etc. or raising
 *    anything --
 *    the whole staleness-window mechanism is dead code. (The real P2.2 CRC
 *    detection instead happens via p2_streammodel_callback(), which *is*
 *    live and is tested below.)
 *
 * 4. ltntstools_tr101290_alarm_raise_all() raises literally every event in
 *    the table (index 1..count-1), including ones with `enabled == 0` in
 *    the default table (E101290_P2_4__PCR_ACCURACY_ERROR,
 *    E101290_P2_5__PTS_ERROR) -- raise() itself never checks the enabled
 *    flag, only the (separate) reporting path does. This means
 *    ltntstools_tr101290_reset_alarms() puts *disabled* events into a
 *    raised internal state too, even though nothing will ever report them
 *    to the user while disabled. Not necessarily wrong (internal state vs.
 *    user-visible reporting are legitimately different concerns), but
 *    worth knowing; test_alarm_raise_all_raises_every_event_regardless_of_enabled()
 *    below documents the actual behavior rather than assuming the
 *    (arguably more intuitive) enabled-only behavior.
 *
 * 5. *** SEVERE, CONFIRMED VIA AddressSanitizer, NOT JUST THEORETICAL ***
 *    tr_events_tbl[] (tr101290-events.c) is a designated-initializer array
 *    literal whose highest filled index is E101290_P2_6__CAT_ERROR -- there
 *    is no entry for E101290_P4_1__UDP_DROPS (Priority 4 is listed in the
 *    header's enum but was apparently never wired into this table). In C,
 *    an array literal like this sizes itself to (highest designated index
 *    + 1), so sizeof(tr_events_tbl)/sizeof(tr_events_tbl[0]) -- what
 *    _event_table_entry_count() returns, confirmed here to be 16 -- is one
 *    short of E101290_MAX (17).
 *
 *    This alone would just be a latent risk for any caller who happens to
 *    reference E101290_P4_1__UDP_DROPS by value. What makes it severe is
 *    that FOUR loops in this module iterate `for (i = 1; i < E101290_MAX; i++)`
 *    over event_tbl -- tr101290.c:83 (ltntstools_tr101290_log_summary()),
 *    tr101290.c:132 (ltntstools_tr101290_threadFunc()'s own main per-tick
 *    alarm-reporting loop), tr101290-events.c:218
 *    (event_processing_disable_all()) and :232 (event_processing_enable_all())
 *    -- all four read event_tbl[16], one element past the end of the
 *    heap allocation from ltntstools_tr101290_event_table_copy().
 *    ltntstools_tr101290_alarm_raise_all() (tr101290-alarms.c), by
 *    contrast, correctly bounds its own loop with
 *    _event_table_entry_count(s) instead of E101290_MAX -- confirming this
 *    is an inconsistency/oversight, not an intentional design choice.
 *
 *    Confirmed with a standalone AddressSanitizer build of this exact test
 *    file (not part of this file's normal, non-ASan build/run target):
 *    test_alloc_free_basic() -- the simplest possible test, alloc()
 *    immediately followed by free() -- reliably (10/10 runs) crashed with
 *    "AddressSanitizer: heap-buffer-overflow ... in ltntstools_tr101290_log_summary
 *    tr101290.c:85", called from ltntstools_tr101290_threadFunc() at
 *    tr101290.c:144. Because ltntstools_tr101290_threadFunc() is the real
 *    background thread every ltntstools_tr101290_alloc() spawns, and it
 *    calls the affected log_summary()/reporting loop on every ~50ms tick
 *    for the entire lifetime of the handle, this out-of-bounds read fires
 *    continuously (tens of times per second) for every real user of this
 *    module, not just callers who explicitly reference
 *    E101290_P4_1__UDP_DROPS. It happens not to corrupt anything this
 *    test suite's assertions observe under a plain (non-ASan) build --
 *    which is exactly why this stayed hidden -- but it is undefined
 *    behavior on every tick, and worth a prompt, dedicated fix.
 *
 *    test_event_table_size_does_not_cover_all_declared_events() below
 *    demonstrates the root cause (the size mismatch itself) safely and
 *    deterministically, rather than actually performing the unsafe access
 *    (whose result would itself be undefined) inside this file's normal
 *    non-ASan test run.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>

#include "libltntstools/tr101290.h"
#include "libltntstools/pat.h"
#include "tr101290-types.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- "light" context: event_tbl only, no threads/subsystems -------- */

static struct ltntstools_tr101290_s *build_light_ctx(void)
{
	struct ltntstools_tr101290_s *s = calloc(1, sizeof(*s));
	s->event_tbl = ltntstools_tr101290_event_table_copy();
	return s;
}

static void free_light_ctx(struct ltntstools_tr101290_s *s)
{
	free(s->event_tbl);
	free(s);
}

/* -------- event table / naming / priority -------- */

static void test_event_table_copy_matches_known_defaults(void)
{
	struct tr_event_s *tbl = ltntstools_tr101290_event_table_copy();
	CHECK(tbl != NULL);

	CHECK(tbl[E101290_P1_1__TS_SYNC_LOSS].enabled == 1);
	CHECK(tbl[E101290_P1_1__TS_SYNC_LOSS].priorityNr == 1);
	CHECK(tbl[E101290_P1_1__TS_SYNC_LOSS].raised == 0);

	CHECK(tbl[E101290_P2_1__TRANSPORT_ERROR].enabled == 1);
	CHECK(tbl[E101290_P2_1__TRANSPORT_ERROR].priorityNr == 2);

	/* P2.4/P2.5 are disabled by default. */
	CHECK(tbl[E101290_P2_4__PCR_ACCURACY_ERROR].enabled == 0);
	CHECK(tbl[E101290_P2_5__PTS_ERROR].enabled == 0);

	free(tbl);
}

static void test_event_name_ascii_known_and_out_of_range(void)
{
	CHECK(strcmp(ltntstools_tr101290_event_name_ascii(E101290_P1_1__TS_SYNC_LOSS), "E101290_P1_1__TS_SYNC_LOSS") == 0);
	CHECK(strcmp(ltntstools_tr101290_event_name_ascii(E101290_P2_2__CRC_ERROR), "E101290_P2_2__CRC_ERROR") == 0);

	/* "The function will always return a string, regardless of input." */
	const char *name = ltntstools_tr101290_event_name_ascii(E101290_MAX);
	CHECK(name != NULL);
	CHECK(strcmp(name, "E101290_UNDEFINED") == 0);
}

static void test_event_priority_known_and_out_of_range(void)
{
	CHECK(ltntstools_tr101290_event_priority(E101290_P1_1__TS_SYNC_LOSS) == 1);
	CHECK(ltntstools_tr101290_event_priority(E101290_P2_1__TRANSPORT_ERROR) == 2);
	CHECK(ltntstools_tr101290_event_priority(E101290_MAX) == 1); /* falls back to UNDEFINED's priority */
}

/* Documents (does not exercise) the bug in file header note #5: proves the
 * root cause -- tr_events_tbl[]'s real size is one short of E101290_MAX --
 * deterministically and safely, without performing the actual unsafe
 * out-of-bounds access that follows from it. */
static void test_event_table_size_does_not_cover_all_declared_events(void)
{
	int actualSize = _event_table_entry_count(NULL);

	CHECK(actualSize == (int)E101290_P4_1__UDP_DROPS);
	CHECK(actualSize != (int)E101290_MAX);

	/* Concretely: E101290_P4_1__UDP_DROPS is a valid index by every
	 * bounds check this module uses (`event >= E101290_MAX`), but is NOT
	 * a valid index into tr_events_tbl (valid indices are
	 * 0..actualSize-1). */
	CHECK((int)E101290_P4_1__UDP_DROPS < (int)E101290_MAX);
	CHECK((int)E101290_P4_1__UDP_DROPS >= actualSize);
}

/* -------- alarm raise / clear / debounce -------- */

static void test_alarm_raise_sets_raised_and_timestamp(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct timeval t0 = { 1700000000, 0 };

	CHECK(s->event_tbl[E101290_P1_1__TS_SYNC_LOSS].raised == 0);

	ltntstools_tr101290_alarm_raise(s, E101290_P1_1__TS_SYNC_LOSS, &t0);

	CHECK(s->event_tbl[E101290_P1_1__TS_SYNC_LOSS].raised == 1);
	CHECK(s->event_tbl[E101290_P1_1__TS_SYNC_LOSS].lastChanged.tv_sec == t0.tv_sec);
	CHECK(s->event_tbl[E101290_P1_1__TS_SYNC_LOSS].arg[0] == 0);

	free_light_ctx(s);
}

static void test_alarm_raise_with_arg_sets_message(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct timeval t0 = { 1700000000, 0 };

	ltntstools_tr101290_alarm_raise_with_arg(s, E101290_P1_6__PID_ERROR, "0x0100 0x0101 ", &t0);

	CHECK(s->event_tbl[E101290_P1_6__PID_ERROR].raised == 1);
	CHECK(strcmp(s->event_tbl[E101290_P1_6__PID_ERROR].arg, "0x0100 0x0101 ") == 0);

	free_light_ctx(s);
}

static void test_alarm_raise_out_of_range_is_safe_noop(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct timeval t0 = { 1700000000, 0 };

	ltntstools_tr101290_alarm_raise(s, E101290_MAX, &t0);
	ltntstools_tr101290_alarm_raise_with_arg(s, E101290_MAX, "x", &t0);
	_tr101290_event_clear(s, E101290_MAX);
	CHECK(1); /* must not crash */

	free_light_ctx(s);
}

/* Regression-documenting test: an alarm, once raised, cannot be cleared
 * again until autoClearAlarmAfterReport (5000ms for every default-enabled
 * event) has elapsed since it was raised -- clear() calls that arrive
 * sooner are silently ignored. */
static void test_alarm_clear_is_debounced_for_autoclear_window(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct timeval t0 = { 1700000000, 0 };

	ltntstools_tr101290_alarm_raise(s, E101290_P1_2__SYNC_BYTE_ERROR, &t0);
	CHECK(s->event_tbl[E101290_P1_2__SYNC_BYTE_ERROR].raised == 1);

	/* 1ms later: well inside the 5000ms debounce window. */
	struct timeval t1 = { 1700000000, 1000 };
	ltntstools_tr101290_alarm_clear(s, E101290_P1_2__SYNC_BYTE_ERROR, &t1);
	CHECK(s->event_tbl[E101290_P1_2__SYNC_BYTE_ERROR].raised == 1); /* still raised */

	/* 6s later: past the 5000ms debounce window. */
	struct timeval t2 = { 1700000006, 0 };
	ltntstools_tr101290_alarm_clear(s, E101290_P1_2__SYNC_BYTE_ERROR, &t2);
	CHECK(s->event_tbl[E101290_P1_2__SYNC_BYTE_ERROR].raised == 0);
	CHECK(s->event_tbl[E101290_P1_2__SYNC_BYTE_ERROR].arg[0] == 0);

	free_light_ctx(s);
}

/* See file header note #4. */
static void test_alarm_raise_all_raises_every_event_regardless_of_enabled(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	s->now.tv_sec = 1700000000;

	ltntstools_tr101290_alarm_raise_all(s);

	int count = _event_table_entry_count(s);
	for (int i = 1; i < count; i++) {
		CHECK(s->event_tbl[i].raised == 1);
	}

	free_light_ctx(s);
}

/* -------- event_should_report -------- */

static void test_event_should_report_true_on_fresh_state_change(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct timeval t0 = { 1700000000, 0 };

	ltntstools_tr101290_alarm_raise(s, E101290_P1_1__TS_SYNC_LOSS, &t0);
	CHECK(ltntstools_tr101290_event_should_report(s, E101290_P1_1__TS_SYNC_LOSS, &t0) == 1);

	free_light_ctx(s);
}

static void test_event_should_report_false_once_already_reported(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct timeval t0 = { 1700000000, 0 };

	ltntstools_tr101290_alarm_raise(s, E101290_P1_1__TS_SYNC_LOSS, &t0);
	/* Mirrors what tr101290.c's own thread loop does right after deciding
	 * to report: bump lastReported to (now + a hair), suppressing
	 * duplicate reports of the same unchanged state. */
	struct timeval reportedAt = { 1700000000, 1000 };
	s->event_tbl[E101290_P1_1__TS_SYNC_LOSS].lastReported = reportedAt;

	CHECK(ltntstools_tr101290_event_should_report(s, E101290_P1_1__TS_SYNC_LOSS, &t0) == 0);

	free_light_ctx(s);
}

static void test_event_should_report_false_when_disabled(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct timeval t0 = { 1700000000, 0 };

	s->event_tbl[E101290_P1_1__TS_SYNC_LOSS].enabled = 0;
	ltntstools_tr101290_alarm_raise(s, E101290_P1_1__TS_SYNC_LOSS, &t0);

	CHECK(ltntstools_tr101290_event_should_report(s, E101290_P1_1__TS_SYNC_LOSS, &t0) == 0);

	free_light_ctx(s);
}

/* -------- timers (platform-dependent; documents actual behavior here) -------- */

static void test_timers_create_arm_disarm_behavior_on_this_platform(void)
{
	struct ltntstools_tr101290_s *s = build_light_ctx();
	struct tr_event_s ev;
	memset(&ev, 0, sizeof(ev));
	ev.id = E101290_P1_1__TS_SYNC_LOSS;

#if defined(__APPLE__)
	/* Per tr101290-timers.c, POSIX interval timers aren't implemented on
	 * Apple platforms yet -- all three calls are documented to return -1. */
	CHECK(ltntstools_tr101290_timers_create(s, &ev) == -1);
	CHECK(ltntstools_tr101290_timers_arm(s, &ev) == -1);
	CHECK(ltntstools_tr101290_timers_disarm(s, &ev) == -1);
#elif defined(__linux__)
	CHECK(ltntstools_tr101290_timers_create(s, &ev) == 0);
	CHECK(ltntstools_tr101290_timers_arm(s, &ev) == 0);
	CHECK(ltntstools_tr101290_timers_disarm(s, &ev) == 0);
#endif

	free_light_ctx(s);
}

/* -------- public API: lifecycle -------- */

static void notify_cb(void *userContext, struct ltntstools_tr101290_alarm_s *array, int count)
{
	free(array); /* caller owns the array per the API contract */
}

static void *alloc_real(void)
{
	void *hdl = NULL;
	int ret = ltntstools_tr101290_alloc(&hdl, notify_cb, NULL);
	if (ret != 0)
		return NULL;
	return hdl;
}

static void test_alloc_free_basic(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	/* See file header note #1: give the background thread time to set
	 * threadRunning before free() checks it. */
	usleep(50 * 1000);

	ltntstools_tr101290_free(hdl);
}

/* -------- public API: event enable/disable -------- */

static int summary_find(struct ltntstools_tr101290_summary_item_s *items, int count, enum ltntstools_tr101290_event_e id)
{
	for (int i = 0; i < count; i++) {
		if (items[i].id == id)
			return i;
	}
	return -1;
}

static void test_event_processing_enable_disable_toggle(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	CHECK(ltntstools_tr101290_event_processing_disable(hdl, E101290_P1_1__TS_SYNC_LOSS) == 0);

	struct ltntstools_tr101290_summary_item_s *items = NULL;
	int count = 0;
	CHECK(ltntstools_tr101290_summary_get(hdl, &items, &count) == 0);
	int idx = summary_find(items, count, E101290_P1_1__TS_SYNC_LOSS);
	CHECK(idx >= 0);
	if (idx >= 0)
		CHECK(items[idx].enabled == 0);
	free(items);

	CHECK(ltntstools_tr101290_event_processing_enable(hdl, E101290_P1_1__TS_SYNC_LOSS) == 0);
	CHECK(ltntstools_tr101290_summary_get(hdl, &items, &count) == 0);
	idx = summary_find(items, count, E101290_P1_1__TS_SYNC_LOSS);
	CHECK(idx >= 0);
	if (idx >= 0)
		CHECK(items[idx].enabled == 1);
	free(items);

	/* Out-of-range event ids are rejected. */
	CHECK(ltntstools_tr101290_event_processing_enable(hdl, E101290_MAX) == -1);
	CHECK(ltntstools_tr101290_event_processing_disable(hdl, E101290_MAX) == -1);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

static void test_event_processing_disable_all_and_enable_all(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	CHECK(ltntstools_tr101290_event_processing_disable_all(hdl) == 0);

	struct ltntstools_tr101290_summary_item_s *items = NULL;
	int count = 0;
	ltntstools_tr101290_summary_get(hdl, &items, &count);
	int allDisabled = 1;
	for (int i = 0; i < count; i++) {
		if (items[i].enabled)
			allDisabled = 0;
	}
	CHECK(allDisabled);
	free(items);

	CHECK(ltntstools_tr101290_event_processing_enable_all(hdl) == 0);
	ltntstools_tr101290_summary_get(hdl, &items, &count);
	int allEnabled = 1;
	for (int i = 0; i < count; i++) {
		if (!items[i].enabled)
			allEnabled = 0;
	}
	CHECK(allEnabled);
	free(items);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

static void test_event_clear_public_api(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	CHECK(ltntstools_tr101290_event_clear(hdl, E101290_P1_1__TS_SYNC_LOSS) == 0);
	CHECK(ltntstools_tr101290_event_clear(hdl, E101290_MAX) == -1);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

/* -------- public API: summary -------- */

static void test_summary_get_returns_all_events_matching_defaults(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	struct ltntstools_tr101290_summary_item_s *items = NULL;
	int count = 0;
	CHECK(ltntstools_tr101290_summary_get(hdl, &items, &count) == 0);
	CHECK(items != NULL);
	/* excludes E101290_UNDEFINED. NOT (int)E101290_MAX - 1: see file
	 * header note #5 -- tr_events_tbl[] is actually one entry shorter
	 * than E101290_MAX implies. */
	CHECK(count == _event_table_entry_count((struct ltntstools_tr101290_s *)hdl) - 1);

	int idx = summary_find(items, count, E101290_P1_1__TS_SYNC_LOSS);
	CHECK(idx >= 0);
	if (idx >= 0) {
		CHECK(items[idx].priorityNr == 1);
		CHECK(items[idx].enabled == 1);
		CHECK(items[idx].raised == 0);
	}

	free(items);
	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

static void test_summary_report_dprintf_produces_output(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	char path[] = "/tmp/tr101290_summary_test_XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);

	CHECK(ltntstools_tr101290_summary_report_dprintf(hdl, fd) == 0);
	close(fd);

	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f) {
		char buf[64];
		size_t n = fread(buf, 1, sizeof(buf), f);
		CHECK(n > 0);
		fclose(f);
	}
	unlink(path);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

/* -------- public API: logging -------- */

static void test_log_enable_creates_file_and_rejects_second_call(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	char path[] = "/tmp/tr101290_log_test_XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd >= 0)
		close(fd);

	CHECK(ltntstools_tr101290_log_enable(hdl, path) == 0);
	/* "Invalid to change the log directory once set." */
	CHECK(ltntstools_tr101290_log_enable(hdl, path) == -1);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);

	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f) {
		char buf[4096];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		CHECK(strstr(buf, "TR101290 Logging started") != NULL);
		CHECK(strstr(buf, "TR101290 Logging stopped") != NULL);
		fclose(f);
	}
	unlink(path);
}

static void test_log_rotate_returns_error(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	/* Documented: "NOT YET SUPPORTED, WILL RETURN ERROR." */
	CHECK(ltntstools_tr101290_log_rotate(hdl) == -1);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

/* -------- public API: reset_alarms -------- */

static void test_reset_alarms_raises_every_event(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	CHECK(ltntstools_tr101290_reset_alarms(hdl) == 0);

	struct ltntstools_tr101290_summary_item_s *items = NULL;
	int count = 0;
	ltntstools_tr101290_summary_get(hdl, &items, &count);
	int allRaised = 1;
	for (int i = 0; i < count; i++) {
		if (!items[i].raised)
			allRaised = 0;
	}
	CHECK(allRaised);
	free(items);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

/* -------- dprintf helpers: smoke tests -------- */

static void test_event_dprintf_and_summary_item_dprintf_do_not_crash(void)
{
	struct ltntstools_tr101290_alarm_s alarm;
	memset(&alarm, 0, sizeof(alarm));
	alarm.id = E101290_P1_1__TS_SYNC_LOSS;
	alarm.priorityNr = 1;
	alarm.raised = 1;
	snprintf(alarm.description, sizeof(alarm.description), "%s", "test");

	struct ltntstools_tr101290_summary_item_s si;
	memset(&si, 0, sizeof(si));
	si.id = E101290_P1_1__TS_SYNC_LOSS;
	si.priorityNr = 1;
	si.enabled = 1;
	si.raised = 0;

	int devnull = open("/dev/null", O_WRONLY);
	CHECK(devnull >= 0);
	if (devnull >= 0) {
		ltntstools_tr101290_event_dprintf(devnull, &alarm);
		ltntstools_tr101290_summary_item_dprintf(devnull, &si);
		close(devnull);
	}
	CHECK(1);
}

/* -------- public API: write() end-to-end, P1.3 PAT error -------- */

static void build_pat_packet(uint8_t *pkt, uint8_t cc)
{
	struct ltntstools_pat_s *pat = ltntstools_pat_alloc();
	pat->transport_stream_id = 1;
	struct ltntstools_pat_program_s *pp = &pat->programs[pat->program_count++];
	memset(pp, 0, sizeof(*pp));
	pp->program_number = 1;
	pp->program_map_PID = 0x100;
	pp->pmt.program_number = 1;
	ltntstools_pat_create_packet_ts(pat, cc, pkt, 188);
	ltntstools_pat_free(pat);
}

static void build_junk_packet(uint8_t *pkt, uint16_t pid, uint8_t cc)
{
	memset(pkt, 0xFF, 188);
	pkt[0] = 0x47;
	pkt[1] = (pid >> 8) & 0x1f;
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10 | (cc & 0x0f); /* payload only, no TEI, no scrambling */
}

/* Deterministic because s->lastPAT starts at the zero timeval on a fresh
 * context, so "no PAT seen in the last 500ms" is trivially true relative
 * to any real/synthetic "now" -- no need to wait on real wall-clock time.
 *
 * The base timestamp is anchored to the real current time (not an
 * arbitrary fixed value) deliberately: ltntstools_tr101290_threadFunc()
 * (the real background thread this test's alloc_real() spins up) has its
 * own independent per-event `nextAlarm` re-raise check
 * (`if (timercmp(&now, &ev->nextAlarm, >=)) alarm_raise(...)`) driven by
 * its own internal gettimeofday(), not by the timestamps this test passes
 * to write(). Anchoring far from real time (e.g. a fixed historical
 * epoch) makes every ev->nextAlarm this test computes already "in the
 * past" relative to that thread's real clock, so it can race in and
 * re-raise the alarm between this test's write() and its summary_get()
 * check -- confirmed flaky (~1/10 runs) before this fix. */
static void test_write_without_pat_raises_pat_error_then_clears_with_good_pat_after_debounce(void)
{
	void *hdl = alloc_real();
	CHECK(hdl != NULL);

	uint8_t junk[3][188];
	build_junk_packet(junk[0], 0x101, 0);
	build_junk_packet(junk[1], 0x102, 0);
	build_junk_packet(junk[2], 0x103, 0);

	struct timeval t0;
	gettimeofday(&t0, NULL);
	CHECK(ltntstools_tr101290_write(hdl, &junk[0][0], 3, &t0) == 3);

	struct ltntstools_tr101290_summary_item_s *items = NULL;
	int count = 0;
	ltntstools_tr101290_summary_get(hdl, &items, &count);
	int idx = summary_find(items, count, E101290_P1_3__PAT_ERROR);
	CHECK(idx >= 0);
	if (idx >= 0)
		CHECK(items[idx].raised == 1);
	free(items);

	/* 6s later (past the 5000ms raise-debounce), a real PAT packet
	 * should clear it. */
	uint8_t pat[188];
	build_pat_packet(pat, 0);
	struct timeval sixSeconds = { 6, 0 };
	struct timeval t1;
	timeradd(&t0, &sixSeconds, &t1);
	CHECK(ltntstools_tr101290_write(hdl, pat, 1, &t1) == 1);

	ltntstools_tr101290_summary_get(hdl, &items, &count);
	idx = summary_find(items, count, E101290_P1_3__PAT_ERROR);
	CHECK(idx >= 0);
	if (idx >= 0)
		CHECK(items[idx].raised == 0);
	free(items);

	usleep(20 * 1000);
	ltntstools_tr101290_free(hdl);
}

int main(void)
{
	test_event_table_copy_matches_known_defaults();
	test_event_name_ascii_known_and_out_of_range();
	test_event_priority_known_and_out_of_range();
	test_event_table_size_does_not_cover_all_declared_events();

	test_alarm_raise_sets_raised_and_timestamp();
	test_alarm_raise_with_arg_sets_message();
	test_alarm_raise_out_of_range_is_safe_noop();
	test_alarm_clear_is_debounced_for_autoclear_window();
	test_alarm_raise_all_raises_every_event_regardless_of_enabled();

	test_event_should_report_true_on_fresh_state_change();
	test_event_should_report_false_once_already_reported();
	test_event_should_report_false_when_disabled();

	test_timers_create_arm_disarm_behavior_on_this_platform();

	test_alloc_free_basic();
	test_event_processing_enable_disable_toggle();
	test_event_processing_disable_all_and_enable_all();
	test_event_clear_public_api();

	test_summary_get_returns_all_events_matching_defaults();
	test_summary_report_dprintf_produces_output();

	test_log_enable_creates_file_and_rejects_second_call();
	test_log_rotate_returns_error();

	test_reset_alarms_raises_every_event();

	test_event_dprintf_and_summary_item_dprintf_do_not_crash();

	test_write_without_pat_raises_pat_error_then_clears_with_good_pat_after_debounce();

	if (g_failures == 0) {
		printf("PASS: all tr101290 tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d tr101290 test(s) failed\n", g_failures);
	return 1;
}
