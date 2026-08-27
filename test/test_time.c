/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/time.c / src/libltntstools/time.h.
 * Builds against ../src/time.c only.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libltntstools/time.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- libltntstools_getTimestamp -------- */

static void test_getTimestamp_known_time(void)
{
	/* 2021-02-03 04:05:06 UTC. Force TZ=UTC for a deterministic
	 * localtime_r() result regardless of the host's timezone. */
	setenv("TZ", "UTC", 1);
	tzset();

	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 2021 - 1900;
	tm.tm_mon = 2 - 1;
	tm.tm_mday = 3;
	tm.tm_hour = 4;
	tm.tm_min = 5;
	tm.tm_sec = 6;
	tm.tm_isdst = -1;
	time_t when = timegm(&tm);

	char buf[32];
	int ret = libltntstools_getTimestamp(buf, sizeof(buf), &when);
	CHECK(ret == 0);
	CHECK(strcmp(buf, "20210203-040506") == 0);
}

static void test_getTimestamp_buflen_too_small_fails(void)
{
	char buf[16];
	time_t when = 0;
	int ret = libltntstools_getTimestamp(buf, 15, &when);
	CHECK(ret == -1);
}

static void test_getTimestamp_buflen_exact_minimum_succeeds(void)
{
	char buf[16];
	time_t when = 0;
	int ret = libltntstools_getTimestamp(buf, 16, &when);
	CHECK(ret == 0);
	CHECK(strlen(buf) == 15);
}

static void test_getTimestamp_null_when_uses_now(void)
{
	time_t before = time(NULL);
	char buf[32];
	int ret = libltntstools_getTimestamp(buf, sizeof(buf), NULL);
	time_t after = time(NULL);
	CHECK(ret == 0);
	CHECK(strlen(buf) == 15);

	/* Parse the formatted string back and confirm it lands within the
	 * [before, after] window, proving NULL really used "now" rather than
	 * stale or garbage data. */
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	int n = sscanf(buf, "%4d%2d%2d-%2d%2d%2d",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min, &tm.tm_sec);
	CHECK(n == 6);
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_isdst = -1;
	time_t parsed = mktime(&tm);
	CHECK(parsed >= before && parsed <= after);
}

/* -------- libltntstools_getTimestamp_seperated -------- */

static void test_getTimestamp_seperated_known_time(void)
{
	setenv("TZ", "UTC", 1);
	tzset();

	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 2021 - 1900;
	tm.tm_mon = 2 - 1;
	tm.tm_mday = 3;
	tm.tm_hour = 4;
	tm.tm_min = 5;
	tm.tm_sec = 6;
	tm.tm_isdst = -1;
	time_t when = timegm(&tm);

	char buf[32];
	int ret = libltntstools_getTimestamp_seperated(buf, sizeof(buf), &when);
	CHECK(ret == 0);
	CHECK(strcmp(buf, "2021-02-03 04:05:06") == 0);
}

static void test_getTimestamp_seperated_buflen_too_small_fails(void)
{
	char buf[16];
	time_t when = 0;
	int ret = libltntstools_getTimestamp_seperated(buf, 15, &when);
	CHECK(ret == -1);
}

static void test_getTimestamp_seperated_null_when_uses_now(void)
{
	time_t before = time(NULL);
	char buf[32];
	int ret = libltntstools_getTimestamp_seperated(buf, sizeof(buf), NULL);
	time_t after = time(NULL);
	CHECK(ret == 0);
	CHECK(strlen(buf) == 19);

	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	int n = sscanf(buf, "%4d-%2d-%2d %2d:%2d:%2d",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min, &tm.tm_sec);
	CHECK(n == 6);
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_isdst = -1;
	time_t parsed = mktime(&tm);
	CHECK(parsed >= before && parsed <= after);
}

/* -------- libltntstools_timespec_diff_ms -------- */

static void test_timespec_diff_ms_no_borrow(void)
{
	struct timespec last_time = { .tv_sec = 10, .tv_nsec = 100000000 };
	struct timespec next_time = { .tv_sec = 12, .tv_nsec = 500000000 };
	/* (12.5 - 10.1)s = 2.4s = 2400ms */
	CHECK(libltntstools_timespec_diff_ms(next_time, last_time) == 2400);
}

static void test_timespec_diff_ms_with_borrow(void)
{
	struct timespec last_time = { .tv_sec = 10, .tv_nsec = 800000000 };
	struct timespec next_time = { .tv_sec = 12, .tv_nsec = 100000000 };
	/* (12.1 - 10.8)s = 1.3s = 1300ms */
	CHECK(libltntstools_timespec_diff_ms(next_time, last_time) == 1300);
}

static void test_timespec_diff_ms_zero(void)
{
	struct timespec t = { .tv_sec = 42, .tv_nsec = 123000000 };
	CHECK(libltntstools_timespec_diff_ms(t, t) == 0);
}

static void test_timespec_diff_ms_negative(void)
{
	struct timespec last_time = { .tv_sec = 12, .tv_nsec = 500000000 };
	struct timespec next_time = { .tv_sec = 10, .tv_nsec = 100000000 };
	/* (10.1 - 12.5)s = -2.4s = -2400ms */
	CHECK(libltntstools_timespec_diff_ms(next_time, last_time) == -2400);
}

static void test_timespec_diff_ms_subsecond_only(void)
{
	struct timespec last_time = { .tv_sec = 5, .tv_nsec = 0 };
	struct timespec next_time = { .tv_sec = 5, .tv_nsec = 250000000 };
	CHECK(libltntstools_timespec_diff_ms(next_time, last_time) == 250);
}

int main(void)
{
	test_getTimestamp_known_time();
	test_getTimestamp_buflen_too_small_fails();
	test_getTimestamp_buflen_exact_minimum_succeeds();
	test_getTimestamp_null_when_uses_now();

	test_getTimestamp_seperated_known_time();
	test_getTimestamp_seperated_buflen_too_small_fails();
	test_getTimestamp_seperated_null_when_uses_now();

	test_timespec_diff_ms_no_borrow();
	test_timespec_diff_ms_with_borrow();
	test_timespec_diff_ms_zero();
	test_timespec_diff_ms_negative();
	test_timespec_diff_ms_subsecond_only();

	if (g_failures == 0) {
		printf("PASS: all time tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d time test(s) failed\n", g_failures);
	return 1;
}
