/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/utils.c / src/utils.h.
 * Builds against ../src/utils.c only. Requires -lpthread.
 *
 * SCOPE NOTE: as of this writing, everything in utils.c except
 * ltnpthread_setname_np() (character_replace, networkInterfaceExists,
 * networkInterfaceList, network_addr_compare, network_stream_ascii) is
 * wrapped in `#if 0` / `#endif` -- dead code, not compiled into the
 * library, and nothing else in the tree calls it. There is nothing to
 * unit test there without first re-enabling that block (a real source
 * change, out of scope here), so this file only covers the one function
 * that's actually live.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "utils.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* On the current thread, with a valid short name, this must succeed. */
static void test_setname_current_thread(void)
{
	int ret = ltnpthread_setname_np(pthread_self(), "klqtest");
	CHECK(ret == 0);

#if defined(__linux__)
	/* On Linux this is a real pthread_setname_np() passthrough --
	 * confirm the name was actually applied. */
	char buf[64] = { 0 };
	CHECK(pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "klqtest") == 0);
#endif
}

#if defined(__APPLE__)
/* On OSX/Darwin the implementation is documented as a no-op ("We don't
 * support thread naming on OSX, yet.") -- it always returns 0 and never
 * dereferences its arguments, so even a bogus thread / NULL name is safe. */
static void test_setname_is_a_noop_on_apple(void)
{
	CHECK(ltnpthread_setname_np((pthread_t)0, NULL) == 0);
	CHECK(ltnpthread_setname_np(pthread_self(), NULL) == 0);
}
#endif

int main(void)
{
	test_setname_current_thread();
#if defined(__APPLE__)
	test_setname_is_a_noop_on_apple();
#endif

	if (g_failures == 0) {
		printf("PASS: all utils tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d utils test(s) failed\n", g_failures);
	return 1;
}
