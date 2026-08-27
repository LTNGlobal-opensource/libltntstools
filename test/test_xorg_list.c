/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/libltntstools/xorg-list.h. Header-only (all
 * inline/macro), no other .c dependency.
 *
 * A REAL maintenance defect was found while writing these tests and has
 * been FIXED: this project ships TWO copies of this file -- the public
 * src/libltntstools/xorg-list.h (tested here, the one any external
 * consumer of this library would #include) and a private src/xorg-list.h
 * that 5 .c files in this codebase (history-metric.c, stats.c,
 * smoother-pcr.c, smoother-rtp.c, throughput_hires.c) actually include via
 * quote-form #include "xorg-list.h" (which resolves to the same-directory
 * private copy, not the public one). The two had silently drifted apart:
 * the public copy defined xorg_list_for_each_entry_reverse(), the private
 * copy did not (confirmed via `diff`, a single-macro difference). Fixed by
 * adding the missing macro to the private copy; the two files are now
 * byte-identical (`diff` exits 0). This is exactly the kind of "vendored
 * file edited in only one of its two locations" drift that's easy to miss
 * and easy to prevent by testing whichever copy is public/canonical, which
 * is what this file does.
 *
 * TWO REAL macro bugs were also found and FIXED (in both copies, to keep
 * them in sync):
 *
 * 1. container_of() (and everything built on it: xorg_list_entry(),
 *    xorg_list_first_entry(), xorg_list_last_entry()) expanded to
 *    `(type *)((char *)(ptr) - offsetof(type, member))` -- missing an
 *    outer pair of parens around the whole expression. `(type *)(EXPR)` is
 *    not itself a postfix-expression in C, so a trailing `->field` on the
 *    macro's result bound to the untyped pointer arithmetic *before* the
 *    cast, not to the cast's result -- confirmed with a real build error
 *    ("member reference base type 'char' is not a structure or union") the
 *    very first time this file tried the natural, idiomatic
 *    `xorg_list_first_entry(&head, struct item_s, list)->value`. The
 *    header's own usage examples always assign the macro's result to a
 *    separate variable first, sidestepping this -- which is presumably why
 *    it went unnoticed. Fixed by wrapping the whole cast-expression in one
 *    more pair of parens (the standard, zero-risk remedy for this class of
 *    C macro pitfall); __container_of() got the identical fix for the same
 *    reason.
 *
 * 2. *** More severe: a real, reproducible crash, not just a compile
 *    error. *** __container_of()'s non-typeof fallback (used
 *    unconditionally in this project's actual build -- HAVE_TYPEOF is
 *    never defined anywhere; there's no config.h and configure.ac never
 *    references it) reads the *value* of the loop iterator variable before
 *    it has ever been assigned (xorg_list_for_each_entry()'s own
 *    documented usage pattern declares the iterator with no initializer).
 *    The arithmetic is designed to mathematically cancel that value out,
 *    but doing so is still undefined behavior per the C standard --  and
 *    the header's own comment already admitted as much ("has undefined
 *    behavior according to the C standard, but it works in many cases").
 *    This session found a case where it does NOT work: a standalone repro
 *    of exactly the header's own top-of-file for_each_entry() example (a
 *    3-element list) reliably crashed (SIGBUS) under `-O2` with this
 *    project's real compiler/platform, while passing cleanly under `-O0` --
 *    textbook optimizer-exploited UB. Since typeof() is fully supported by
 *    both gcc and clang (the only two compilers realistically used to
 *    build this project) as the portable `__typeof__` spelling, fixed by
 *    auto-detecting compiler support (`__GNUC__`/`__clang__`) instead of
 *    depending on the never-set HAVE_TYPEOF, so the always-safe typeof()
 *    path is the one actually used in practice. The UB fallback is kept as
 *    a last resort for any hypothetical third compiler.
 *    test_for_each_entry_*() and test_append_adds_to_end() below (any test
 *    using xorg_list_for_each_entry* with an uninitialized iterator, which
 *    is all of them, matching the header's own documented usage) are the
 *    regression coverage; this file's suite reliably crashed under `-O2`
 *    before this fix and is now stress-tested (20 consecutive runs) and
 *    AddressSanitizer/UBSan-clean after it.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/xorg-list.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

struct item_s
{
	int value;
	struct xorg_list list;
};

/* -------- init / is_empty -------- */

static void test_init_creates_empty_self_referencing_list(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	CHECK(head.next == &head);
	CHECK(head.prev == &head);
	CHECK(xorg_list_is_empty(&head));
}

/* -------- add (prepend) / append -------- */

static void test_add_prepends_to_front(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s a = { .value = 1 };
	struct item_s b = { .value = 2 };

	xorg_list_add(&a.list, &head);
	CHECK(!xorg_list_is_empty(&head));
	CHECK(xorg_list_first_entry(&head, struct item_s, list)->value == 1);

	xorg_list_add(&b.list, &head); /* prepend again -> b becomes first */
	CHECK(xorg_list_first_entry(&head, struct item_s, list)->value == 2);
	CHECK(xorg_list_last_entry(&head, struct item_s, list)->value == 1);
}

static void test_append_adds_to_end(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s a = { .value = 1 };
	struct item_s b = { .value = 2 };
	struct item_s c = { .value = 3 };

	xorg_list_append(&a.list, &head);
	xorg_list_append(&b.list, &head);
	xorg_list_append(&c.list, &head);

	CHECK(xorg_list_first_entry(&head, struct item_s, list)->value == 1);
	CHECK(xorg_list_last_entry(&head, struct item_s, list)->value == 3);

	int expected[3] = { 1, 2, 3 };
	int i = 0;
	struct item_s *e;
	xorg_list_for_each_entry(e, &head, list) {
		CHECK(e->value == expected[i++]);
	}
	CHECK(i == 3);
}

/* -------- del -------- */

static void test_del_removes_and_reinitializes_entry(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s a = { .value = 1 };
	struct item_s b = { .value = 2 };
	struct item_s c = { .value = 3 };
	xorg_list_append(&a.list, &head);
	xorg_list_append(&b.list, &head);
	xorg_list_append(&c.list, &head);

	xorg_list_del(&b.list);

	/* Removed from the original list. */
	int expected[2] = { 1, 3 };
	int i = 0;
	struct item_s *e;
	xorg_list_for_each_entry(e, &head, list) {
		CHECK(e->value == expected[i++]);
	}
	CHECK(i == 2);

	/* Left as its own empty (self-referencing) list -- safe to reuse. */
	CHECK(xorg_list_is_empty(&b.list));
	CHECK(b.list.next == &b.list);
	CHECK(b.list.prev == &b.list);

	/* Deleting the last real element leaves the head empty again. */
	xorg_list_del(&a.list);
	xorg_list_del(&c.list);
	CHECK(xorg_list_is_empty(&head));
}

static void test_del_reinserted_entry_works(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s a = { .value = 1 };
	xorg_list_append(&a.list, &head);
	xorg_list_del(&a.list);

	/* A deleted-then-reinserted node must behave correctly, not carry
	 * stale pointers from its previous membership. */
	xorg_list_append(&a.list, &head);
	CHECK(!xorg_list_is_empty(&head));
	CHECK(xorg_list_first_entry(&head, struct item_s, list)->value == 1);
}

/* -------- first_entry / last_entry -------- */

static void test_first_and_last_entry_single_element(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s a = { .value = 42 };
	xorg_list_append(&a.list, &head);

	CHECK(xorg_list_first_entry(&head, struct item_s, list) == &a);
	CHECK(xorg_list_last_entry(&head, struct item_s, list) == &a);
}

/* -------- for_each_entry / for_each_entry_reverse -------- */

static void test_for_each_entry_empty_list_iterates_zero_times(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	int count = 0;
	struct item_s *e;
	xorg_list_for_each_entry(e, &head, list) {
		count++;
	}
	CHECK(count == 0);
}

static void test_for_each_entry_reverse_visits_in_reverse_order(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s a = { .value = 1 };
	struct item_s b = { .value = 2 };
	struct item_s c = { .value = 3 };
	xorg_list_append(&a.list, &head);
	xorg_list_append(&b.list, &head);
	xorg_list_append(&c.list, &head);

	int expected[3] = { 3, 2, 1 };
	int i = 0;
	struct item_s *e;
	xorg_list_for_each_entry_reverse(e, &head, list) {
		CHECK(e->value == expected[i++]);
	}
	CHECK(i == 3);
}

/* -------- for_each_entry_safe -------- */

static void test_for_each_entry_safe_allows_deletion_during_iteration(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s items[5];
	for (int i = 0; i < 5; i++) {
		items[i].value = i;
		xorg_list_append(&items[i].list, &head);
	}

	/* Delete every even-valued item while iterating. */
	struct item_s *e, *tmp;
	xorg_list_for_each_entry_safe(e, tmp, &head, list) {
		if (e->value % 2 == 0) {
			xorg_list_del(&e->list);
		}
	}

	int expected[2] = { 1, 3 };
	int i = 0;
	xorg_list_for_each_entry(e, &head, list) {
		CHECK(e->value == expected[i++]);
	}
	CHECK(i == 2);

	/* The deleted nodes are each their own valid empty list. */
	CHECK(xorg_list_is_empty(&items[0].list));
	CHECK(xorg_list_is_empty(&items[2].list));
	CHECK(xorg_list_is_empty(&items[4].list));
}

static void test_for_each_entry_safe_allows_deleting_all_entries(void)
{
	struct xorg_list head;
	xorg_list_init(&head);

	struct item_s items[3];
	for (int i = 0; i < 3; i++) {
		items[i].value = i;
		xorg_list_append(&items[i].list, &head);
	}

	struct item_s *e, *tmp;
	int visited = 0;
	xorg_list_for_each_entry_safe(e, tmp, &head, list) {
		xorg_list_del(&e->list);
		visited++;
	}

	CHECK(visited == 3);
	CHECK(xorg_list_is_empty(&head));
}

/* -------- xorg_list_entry / container_of -------- */

static void test_xorg_list_entry_matches_container_of(void)
{
	struct item_s a = { .value = 99 };
	struct item_s *recovered = xorg_list_entry(&a.list, struct item_s, list);
	CHECK(recovered == &a);
}

/* -------- NULL-terminated list interface -------- */

struct nt_item_s
{
	int value;
	struct nt_item_s *next;
};

static void test_nt_list_init_and_next(void)
{
	struct nt_item_s a = { .value = 1, .next = (struct nt_item_s *)0xdeadbeef };
	nt_list_init(&a, next);
	CHECK(a.next == NULL);
	CHECK(nt_list_next(&a, next) == NULL);
}

static void test_nt_list_for_each_entry_traverses_in_order(void)
{
	struct nt_item_s c = { .value = 3, .next = NULL };
	struct nt_item_s b = { .value = 2, .next = &c };
	struct nt_item_s a = { .value = 1, .next = &b };

	int expected[3] = { 1, 2, 3 };
	int i = 0;
	struct nt_item_s *it;
	nt_list_for_each_entry(it, &a, next) {
		CHECK(it->value == expected[i++]);
	}
	CHECK(i == 3);
}

static void test_nt_list_append_attaches_at_tail(void)
{
	struct nt_item_s newNode = { .value = 99, .next = NULL };
	struct nt_item_s b = { .value = 2, .next = NULL };
	struct nt_item_s a = { .value = 1, .next = &b };

	nt_list_append(&newNode, &a, struct nt_item_s, next);

	CHECK(a.next == &b);
	CHECK(b.next == &newNode);
	CHECK(newNode.next == NULL);
}

/* Regression-worthy: nt_list_insert()'s implementation calls nt_list_append()
 * with its own two arguments SWAPPED relative to what a casual read might
 * expect (append(entry_to_attach, list_to_walk, ...) vs. the reversed roles
 * needed here) -- tracing it confirms it's correct, but it's exactly the
 * kind of macro that's easy to silently break, making it worth pinning
 * down precisely. */
static void test_nt_list_insert_splices_after_head(void)
{
	struct nt_item_s tail = { .value = 20, .next = NULL };
	struct nt_item_s head = { .value = 10, .next = &tail };

	struct nt_item_s e1 = { .value = 2, .next = NULL };
	struct nt_item_s e0 = { .value = 1, .next = &e1 };

	/* Expected result: head(10) -> e0(1) -> e1(2) -> tail(20) */
	nt_list_insert(&e0, &head, struct nt_item_s, next);

	CHECK(head.next == &e0);
	CHECK(e0.next == &e1);
	CHECK(e1.next == &tail);
	CHECK(tail.next == NULL);

	int expected[4] = { 10, 1, 2, 20 };
	int i = 0;
	struct nt_item_s *it;
	nt_list_for_each_entry(it, &head, next) {
		CHECK(it->value == expected[i++]);
	}
	CHECK(i == 4);
}

static void test_nt_list_del_head_middle_tail(void)
{
	struct nt_item_s c = { .value = 3, .next = NULL };
	struct nt_item_s b = { .value = 2, .next = &c };
	struct nt_item_s a = { .value = 1, .next = &b };
	struct nt_item_s *list = &a;

	/* Delete the middle element. */
	nt_list_del(&b, list, struct nt_item_s, next);
	CHECK(list == &a);
	CHECK(a.next == &c);
	CHECK(b.next == NULL); /* re-initialized as its own null-terminated list */

	/* Delete the head. */
	nt_list_del(&a, list, struct nt_item_s, next);
	CHECK(list == &c);
	CHECK(a.next == NULL);

	/* Delete the (now only) tail/head element. */
	nt_list_del(&c, list, struct nt_item_s, next);
	CHECK(list == NULL);
}

static void test_nt_list_del_safe_traversal(void)
{
	struct nt_item_s items[4];
	for (int i = 0; i < 4; i++) {
		items[i].value = i;
		items[i].next = (i < 3) ? &items[i + 1] : NULL;
	}
	struct nt_item_s *list = &items[0];

	struct nt_item_s *e, *tmp;
	nt_list_for_each_entry_safe(e, tmp, list, next) {
		if (e->value % 2 == 0) {
			nt_list_del(e, list, struct nt_item_s, next);
		}
	}

	int expected[2] = { 1, 3 };
	int i = 0;
	struct nt_item_s *it;
	nt_list_for_each_entry(it, list, next) {
		CHECK(it->value == expected[i++]);
	}
	CHECK(i == 2);
}

int main(void)
{
	test_init_creates_empty_self_referencing_list();

	test_add_prepends_to_front();
	test_append_adds_to_end();

	test_del_removes_and_reinitializes_entry();
	test_del_reinserted_entry_works();

	test_first_and_last_entry_single_element();

	test_for_each_entry_empty_list_iterates_zero_times();
	test_for_each_entry_reverse_visits_in_reverse_order();

	test_for_each_entry_safe_allows_deletion_during_iteration();
	test_for_each_entry_safe_allows_deleting_all_entries();

	test_xorg_list_entry_matches_container_of();

	test_nt_list_init_and_next();
	test_nt_list_for_each_entry_traverses_in_order();
	test_nt_list_append_attaches_at_tail();
	test_nt_list_insert_splices_after_head();
	test_nt_list_del_head_middle_tail();
	test_nt_list_del_safe_traversal();

	if (g_failures == 0) {
		printf("PASS: all xorg-list tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d xorg-list test(s) failed\n", g_failures);
	return 1;
}
