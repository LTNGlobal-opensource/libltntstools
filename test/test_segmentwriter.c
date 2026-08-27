/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/segmentwriter.c / src/libltntstools/segmentwriter.h.
 * Builds against ../src/segmentwriter.c plus ../src/kl-queue.c (the
 * internal work queue). Links -lpthread.
 *
 * This is a real threaded file writer: a background thread polls its queue
 * every 10ms and, on first write, lazily opens a real file on disk named
 * "<prefix>-<timestamp><suffix>". These tests create a fresh temp directory
 * per run (mkdtemp), write through the real API, and poll (with generous
 * timeouts) for the background thread to open the file and flush queued
 * data, then verify the actual bytes on disk. The temp directory and its
 * contents are removed at the end of each test.
 *
 * TWO REAL bugs were found while writing these tests and have been FIXED:
 *
 * 1. The same use-after-free race already found and fixed in
 *    smoother-pcr.c, probes.c and smoother-rtp.c: ltntstools_segmentwriter_alloc()
 *    spawned its background thread and returned pthread_create()'s result
 *    directly, while threadRunning was only ever set by the thread itself
 *    after it started running. ltntstools_segmentwriter_free() gates its
 *    "wait for the thread to terminate" logic on threadRunning, so
 *    alloc() immediately followed by free() (no delay -- exactly
 *    test_alloc_free_basic() below) could skip the wait entirely and let
 *    the thread dereference an already-freed context. Fixed by setting
 *    threadRunning = 1 in the parent, synchronously, immediately after a
 *    successful pthread_create() (not unconditionally before it, which
 *    would instead hang free() forever if pthread_create() itself ever
 *    failed -- the same refinement was applied to the other three files'
 *    fixes for this bug while writing this file's tests).
 *
 * 2. ltntstools_segmentwriter_get_current_filename() used strncpy(dst,
 *    filename, lengthBytes), which does not null-terminate dst when the
 *    real filename is >= lengthBytes -- returning a non-C-string to the
 *    caller. Confirmed via repro: a long filenamePrefix with a small
 *    lengthBytes produced a dst with no trailing NUL before the fix.
 *    Fixed by using snprintf(), which always null-terminates (given
 *    lengthBytes > 0) and truncates safely.
 *    test_get_current_filename_truncates_and_null_terminates() below is
 *    the regression test.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>

#include "libltntstools/segmentwriter.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- temp directory helpers -------- */

static void make_tmp_prefix(char *dirOut, size_t dirLen, char *prefixOut, size_t prefixLen)
{
	char tmpl[] = "/tmp/segmentwriter_test_XXXXXX";
	char *dir = mkdtemp(tmpl);
	assert(dir != NULL);
	snprintf(dirOut, dirLen, "%s", dir);
	snprintf(prefixOut, prefixLen, "%s/seg", dir);
}

static void cleanup_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	char path[1024];
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	closedir(d);
	rmdir(dir);
}

/* -------- polling helpers -------- */

static int wait_for(int (*pred)(void *), void *arg, int timeoutMs)
{
	struct timeval start, now;
	gettimeofday(&start, NULL);
	while (1) {
		if (pred(arg))
			return 1;
		gettimeofday(&now, NULL);
		int64_t elapsedMs = (now.tv_sec - start.tv_sec) * 1000LL + (now.tv_usec - start.tv_usec) / 1000LL;
		if (elapsedMs >= timeoutMs)
			return 0;
		usleep(2 * 1000);
	}
}

static int pred_recording_size_ge(void *arg)
{
	void **a = (void **)arg;
	void *hdl = a[0];
	int64_t want = *(int64_t *)a[1];
	int64_t got = ltntstools_segmentwriter_get_recording_size(hdl);
	return got >= want;
}

static int wait_for_recording_size_ge(void *hdl, int64_t want, int timeoutMs)
{
	void *arg[2] = { hdl, &want };
	return wait_for(pred_recording_size_ge, arg, timeoutMs);
}

static int pred_queue_depth_zero(void *arg)
{
	return ltntstools_segmentwriter_get_queue_depth(arg) == 0;
}

static int wait_for_queue_depth_zero(void *hdl, int timeoutMs)
{
	return wait_for(pred_queue_depth_zero, hdl, timeoutMs);
}

/* -------- lifecycle -------- */

static void test_alloc_free_basic(void)
{
	/* Stress the alloc()-immediately-followed-by-free() pattern that used
	 * to race (see file header). */
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	for (int i = 0; i < 20; i++) {
		void *hdl = NULL;
		CHECK(ltntstools_segmentwriter_alloc(&hdl, prefix, ".ts", SEGMENTEDWRITER_SINGLE_FILE) == 0);
		CHECK(hdl != NULL);
		ltntstools_segmentwriter_free(hdl);
	}

	cleanup_dir(dir);
}

static void test_getters_before_any_write_return_negative(void)
{
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	void *hdl = NULL;
	ltntstools_segmentwriter_alloc(&hdl, prefix, ".ts", SEGMENTEDWRITER_SINGLE_FILE);

	char fn[64];
	CHECK(ltntstools_segmentwriter_get_current_filename(hdl, fn, sizeof(fn)) == -1);
	CHECK(ltntstools_segmentwriter_get_freespace_pct(hdl) == -1);
	CHECK(ltntstools_segmentwriter_get_segment_count(hdl) == -1);
	CHECK(ltntstools_segmentwriter_get_recording_size(hdl) == -1);
	CHECK(ltntstools_segmentwriter_get_recording_start_time(hdl) == -1);

	ltntstools_segmentwriter_free(hdl);
	cleanup_dir(dir);
}

/* -------- write() creates a file with correct content -------- */

static void test_write_creates_file_with_header_and_data(void)
{
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	void *hdl = NULL;
	CHECK(ltntstools_segmentwriter_alloc(&hdl, prefix, ".ts", SEGMENTEDWRITER_SINGLE_FILE) == 0);

	uint8_t header[8] = { 'H', 'E', 'A', 'D', 'E', 'R', '!', '!' };
	CHECK(ltntstools_segmentwriter_set_header(hdl, header, sizeof(header)) == 0);

	uint8_t payload[16];
	for (int i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (uint8_t)i;
	CHECK(ltntstools_segmentwriter_write(hdl, payload, sizeof(payload)) == (ssize_t)sizeof(payload));

	time_t before = time(NULL);
	int64_t expectedSize = sizeof(header) + sizeof(payload);
	CHECK(wait_for_recording_size_ge(hdl, expectedSize, 2000));
	time_t after = time(NULL);

	CHECK(ltntstools_segmentwriter_get_recording_size(hdl) == expectedSize);
	CHECK(ltntstools_segmentwriter_get_segment_count(hdl) == 1);

	time_t start = ltntstools_segmentwriter_get_recording_start_time(hdl);
	CHECK(start >= before && start <= after);

	double pct = ltntstools_segmentwriter_get_freespace_pct(hdl);
	CHECK(pct >= 0.0 && pct <= 100.0);

	char fn[256];
	CHECK(ltntstools_segmentwriter_get_current_filename(hdl, fn, sizeof(fn)) == 0);
	CHECK(strncmp(fn, prefix, strlen(prefix)) == 0);
	CHECK(strstr(fn, ".ts") != NULL);

	/* free() closes (and thus flushes) the writer's own FILE*. Reading
	 * the file back through a separate fopen() beforehand would race
	 * stdio's internal write buffering on the writer's still-open fh. */
	ltntstools_segmentwriter_free(hdl);

	/* Verify the actual bytes on disk: header followed by payload. */
	FILE *f = fopen(fn, "rb");
	CHECK(f != NULL);
	if (f) {
		uint8_t readback[64];
		size_t n = fread(readback, 1, sizeof(readback), f);
		CHECK((int64_t)n == expectedSize);
		CHECK(memcmp(readback, header, sizeof(header)) == 0);
		CHECK(memcmp(readback + sizeof(header), payload, sizeof(payload)) == 0);
		fclose(f);
	}
	cleanup_dir(dir);
}

static void test_write_without_header_writes_data_only(void)
{
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	void *hdl = NULL;
	ltntstools_segmentwriter_alloc(&hdl, prefix, NULL /* -> defaults to .ts */, SEGMENTEDWRITER_SINGLE_FILE);

	uint8_t payload[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	ltntstools_segmentwriter_write(hdl, payload, sizeof(payload));

	CHECK(wait_for_recording_size_ge(hdl, sizeof(payload), 2000));
	CHECK(ltntstools_segmentwriter_get_recording_size(hdl) == (int64_t)sizeof(payload));

	char fn[256];
	ltntstools_segmentwriter_get_current_filename(hdl, fn, sizeof(fn));
	CHECK(strstr(fn, ".ts") != NULL); /* default suffix applied */

	ltntstools_segmentwriter_free(hdl);
	cleanup_dir(dir);
}

static void test_set_header_replaces_previous_header(void)
{
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	void *hdl = NULL;
	ltntstools_segmentwriter_alloc(&hdl, prefix, ".ts", SEGMENTEDWRITER_SINGLE_FILE);

	uint8_t headerA[4] = { 'A', 'A', 'A', 'A' };
	uint8_t headerB[6] = { 'B', 'B', 'B', 'B', 'B', 'B' };
	ltntstools_segmentwriter_set_header(hdl, headerA, sizeof(headerA));
	ltntstools_segmentwriter_set_header(hdl, headerB, sizeof(headerB)); /* replaces A */

	uint8_t payload[3] = { 'x', 'y', 'z' };
	ltntstools_segmentwriter_write(hdl, payload, sizeof(payload));

	int64_t expectedSize = sizeof(headerB) + sizeof(payload);
	CHECK(wait_for_recording_size_ge(hdl, expectedSize, 2000));
	CHECK(ltntstools_segmentwriter_get_recording_size(hdl) == expectedSize);

	char fn[256];
	ltntstools_segmentwriter_get_current_filename(hdl, fn, sizeof(fn));
	ltntstools_segmentwriter_free(hdl); /* flushes fh before we read it back */

	FILE *f = fopen(fn, "rb");
	CHECK(f != NULL);
	if (f) {
		uint8_t readback[16];
		size_t n = fread(readback, 1, sizeof(readback), f);
		CHECK((int64_t)n == expectedSize);
		CHECK(memcmp(readback, headerB, sizeof(headerB)) == 0);
		fclose(f);
	}

	cleanup_dir(dir);
}

/* -------- object_alloc / object_write path -------- */

static void test_object_alloc_write_path(void)
{
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	void *hdl = NULL;
	ltntstools_segmentwriter_alloc(&hdl, prefix, ".ts", SEGMENTEDWRITER_SINGLE_FILE);

	void *obj = NULL;
	uint8_t *dst = NULL;
	CHECK(ltntstools_segmentwriter_object_alloc(hdl, 5, &obj, &dst) == 0);
	CHECK(obj != NULL);
	CHECK(dst != NULL);
	memcpy(dst, "ABCDE", 5);

	CHECK(ltntstools_segmentwriter_object_write(hdl, obj) == 5);

	CHECK(wait_for_recording_size_ge(hdl, 5, 2000));
	CHECK(ltntstools_segmentwriter_get_recording_size(hdl) == 5);

	char fn[256];
	ltntstools_segmentwriter_get_current_filename(hdl, fn, sizeof(fn));
	ltntstools_segmentwriter_free(hdl); /* flushes fh before we read it back */

	FILE *f = fopen(fn, "rb");
	CHECK(f != NULL);
	if (f) {
		char readback[8] = { 0 };
		size_t n = fread(readback, 1, sizeof(readback), f);
		CHECK(n == 5);
		CHECK(memcmp(readback, "ABCDE", 5) == 0);
		fclose(f);
	}

	cleanup_dir(dir);
}

/* -------- queue drains -------- */

static void test_queue_depth_reaches_zero_after_writes(void)
{
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	void *hdl = NULL;
	ltntstools_segmentwriter_alloc(&hdl, prefix, ".ts", SEGMENTEDWRITER_SINGLE_FILE);

	uint8_t payload[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
	for (int i = 0; i < 20; i++) {
		ltntstools_segmentwriter_write(hdl, payload, sizeof(payload));
	}

	CHECK(wait_for_queue_depth_zero(hdl, 2000));
	CHECK(ltntstools_segmentwriter_get_recording_size(hdl) == 20 * (int64_t)sizeof(payload));

	ltntstools_segmentwriter_free(hdl);
	cleanup_dir(dir);
}

/* -------- segmented mode: first segment behaves the same -------- */

static void test_segmented_mode_creates_first_segment_correctly(void)
{
	char dir[256], prefix[300];
	make_tmp_prefix(dir, sizeof(dir), prefix, sizeof(prefix));

	void *hdl = NULL;
	CHECK(ltntstools_segmentwriter_alloc(&hdl, prefix, ".ts", SEGMENTEDWRITER_SEGMENTED) == 0);

	uint8_t payload[6] = { 'S', 'E', 'G', 'M', 'N', 'T' };
	ltntstools_segmentwriter_write(hdl, payload, sizeof(payload));

	CHECK(wait_for_recording_size_ge(hdl, sizeof(payload), 2000));
	CHECK(ltntstools_segmentwriter_get_segment_count(hdl) == 1);
	CHECK(ltntstools_segmentwriter_get_recording_size(hdl) == (int64_t)sizeof(payload));

	ltntstools_segmentwriter_free(hdl);
	cleanup_dir(dir);
}

/* -------- regression: get_current_filename() null-terminates -------- */

static void test_get_current_filename_truncates_and_null_terminates(void)
{
	char dir[256], longPrefix[512];
	{
		char tmpl[] = "/tmp/segmentwriter_test_XXXXXX";
		char *d = mkdtemp(tmpl);
		assert(d != NULL);
		snprintf(dir, sizeof(dir), "%s", d);
	}
	/* A deliberately long prefix so "<prefix>-<ts><suffix>" comfortably
	 * exceeds the small destination buffer below. */
	snprintf(longPrefix, sizeof(longPrefix), "%s/this-is-a-deliberately-long-recording-filename-prefix", dir);

	void *hdl = NULL;
	ltntstools_segmentwriter_alloc(&hdl, longPrefix, ".ts", SEGMENTEDWRITER_SINGLE_FILE);

	uint8_t payload[2] = { 1, 2 };
	ltntstools_segmentwriter_write(hdl, payload, sizeof(payload));
	CHECK(wait_for_recording_size_ge(hdl, sizeof(payload), 2000));

	char small[10];
	memset(small, 0x7f, sizeof(small)); /* sentinel: not a valid C string byte */
	CHECK(ltntstools_segmentwriter_get_current_filename(hdl, small, sizeof(small)) == 0);

	/* Must be a properly terminated, safely truncated C string, not a
	 * sizeof(small)-byte blob with no trailing NUL. */
	CHECK(memchr(small, '\0', sizeof(small)) != NULL);
	CHECK(strlen(small) <= sizeof(small) - 1);

	ltntstools_segmentwriter_free(hdl);
	cleanup_dir(dir);
}

int main(void)
{
	test_alloc_free_basic();
	test_getters_before_any_write_return_negative();

	test_write_creates_file_with_header_and_data();
	test_write_without_header_writes_data_only();
	test_set_header_replaces_previous_header();

	test_object_alloc_write_path();
	test_queue_depth_reaches_zero_after_writes();

	test_segmented_mode_creates_first_segment_correctly();

	test_get_current_filename_truncates_and_null_terminates();

	if (g_failures == 0) {
		printf("PASS: all segmentwriter tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d segmentwriter test(s) failed\n", g_failures);
	return 1;
}
