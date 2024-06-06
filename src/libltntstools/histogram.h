/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

/* The original version of this code lives in the ltntstools project,
 * so please reflect any modifications 'upstream' if this code gets
 * pulled into other projects.
 */

/* Basic histogram facility geared towards video use cases, where
 * buckets span from 1-Nms, and the finest granularity is 1ms.
 * We intensionally tradeoff a large amount of ram for fast
 * bucket update access. The default video use case takes 1MB
 * of ram to hold 16,000 buckets.
 * Generally you'd isolate all access to the histogram to a single thread.
 *
 * Use case: Measuring frame arrival times from SDI capture hardware.
 *
 *   struct ltn_histogram_s *hdl;
 *   ltn_histogram_alloc_video_defaults(&hdl, "frame arrival times");
 *
 *   // Update the histogram every time a new frame arrives.
 *   ltn_histogram_interval_update(hdl);
 *
 *   // Whenever you see fit, print the histogram content:
 *   ltn_histogram_interval_print(STDOUT_FILENO, hdl, 0);
 *
 *   // Free the memory allocations when you're done.
 *   ltn_histogram_free(hdl);
 * 
 *
 * Use case: Measure the amount of time a specific piece of processing takes,
 *           for example, measureing a video frame colorspace conversion.
 *   struct ltn_histogram_s *hdl;
 *   ltn_histogram_alloc_video_defaults(&hdl, "GOP compression time");
 * 
 *   // Perform as many sample measurements as your need.
 *   ltn_histogram_sample_begin(hdl)
 *   ..... do some processing work here, we'll measure performance.
 *   ltn_histogram_sample_end(hdl)
 *
 *   // Assuming this is called multiple times per second,
 *   // every four seconds print the histogram content to console.
 *   ltn_histogram_interval_print(STDOUT_FILENO, hdl, 4);
 *
 *   // Free the memory allocations when you're done.
 *   ltn_histogram_free(hdl);
 * 
 * 
 * Use case: Measuring frame encode time in a cumulative summary,
 *           for example measuring how long it took to compress a gop.
 *
 *   struct ltn_histogram_s *hdl;
 *   ltn_histogram_alloc_video_defaults(&hdl, "CSC conversion time");
 *
 *   ltn_histogram_reset(hdl);
 *
 *   // At the start of a cumulative period, reset any counters, such as when
 *   // the GOP begins.
 *   ltn_histogram_cumulative_initialize(hdl)
 *
 *   // Measure the time it takes to compress each slice, for each and every slice in
 *   // the GOP.
 *   ltn_histogram_cumulative_begin(hdl);
 *   ..... do some processing work here, we'll measure performance.
 *   ltn_histogram_cumulative_end(hdl);
 *
 *   // Finally, when the gop is complete, flush the cumulative value into
 *   // the hisogram buckets. Don't forget to call _initialize() to reset any values
 *   // for the next GOP.
 *   ltn_histogram_cumulative_finalize(hdl);
 *   
 */

#ifndef LTN_HISTOGRAM_H
#define LTN_HISTOGRAM_H

#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <libltntstools/timeval.h>

struct ltn_histogram_bucket_s
{
	uint64_t count;
	struct timeval lastUpdate;
};

struct ltn_histogram_s
{
	char     name[128];
	uint64_t minValMs; /* Minimum / Maximum bucket values in ms */
	uint64_t maxValMs;
	uint64_t bucketMissCount; /* Total instances where a value was not within the valid bucket range. */
	uint32_t bucketCount;

	struct ltn_histogram_bucket_s *buckets;

	/* Interval related hisograms */
	struct timeval intervalLast;

	/* Cumulative histograms */
	uint64_t cumulativeMs;
	struct timeval cumulativeLast;

	/* Per-unit sample histograms */
	uint64_t sampleMs;
	struct timeval sampleLast;

	/* Helper mechism for printing the histogram routinely. */
	struct timeval printLast;
	struct timeval printSummaryLast;

	/* Only supported on 64bit platforms. */
	__int128 totalCount; /* Sum of all buckets */
};

/* Compare time T1 to T2. */
static __inline__ int _compareTime(struct timeval *t1, struct timeval *t2)
{
	if (t1->tv_sec > t2->tv_sec)
		return 1;
	if (t1->tv_sec < t2->tv_sec)
		return -1;

	/* Seconds are identical, compare usecs */
	if (t1->tv_usec > t2->tv_usec)
		return 1;
	if (t1->tv_usec < t2->tv_usec)
		return -1;

	/* Identical. */
	return 0;
}

static __inline__ struct ltn_histogram_bucket_s *ltn_histogram_bucket(struct ltn_histogram_s *ctx, uint32_t ms)
{
	return ctx->buckets + (ms - ctx->minValMs);
}

__inline__ void ltn_histogram_reset(struct ltn_histogram_s *ctx)
{
	memset(ctx->buckets, 0, sizeof(struct ltn_histogram_bucket_s) * ctx->bucketCount);
	gettimeofday(&ctx->intervalLast, NULL);
	ctx->bucketMissCount = 0;
	ctx->cumulativeMs = 0;
	ctx->totalCount = 0;
}

static __inline__ void ltn_histogram_free(struct ltn_histogram_s *ctx)
{
	if (ctx->buckets)
		free(ctx->buckets);
	free(ctx);
}

static __inline__ int ltn_histogram_alloc(struct ltn_histogram_s **handle, const char *name, uint64_t minValMs, uint64_t maxValMs)
{
	*handle = NULL;

	if (minValMs == maxValMs)
		return -1;
	if (maxValMs < minValMs)
		return -1;
	if (!maxValMs)
		return -1;
	if (!name)
		return -1;
	
	struct ltn_histogram_s *ctx = (struct ltn_histogram_s *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->minValMs = minValMs;
	ctx->maxValMs = maxValMs;
	ctx->bucketCount = maxValMs - minValMs;
	strncpy(ctx->name, name, sizeof(ctx->name));
	gettimeofday(&ctx->intervalLast, NULL);

	ctx->buckets = (struct ltn_histogram_bucket_s *)calloc(ctx->bucketCount, sizeof(struct ltn_histogram_s));
	if (!ctx->buckets)
		return -1;

	ltn_histogram_reset(ctx);

	*handle = ctx;
	return 0;
}

static __inline__ int ltn_histogram_alloc_video_defaults(struct ltn_histogram_s **handle, const char *name)
{
	return ltn_histogram_alloc(handle, name, 0, 16 * 1000);
}

static __inline__ int ltn_histogram_interval_update_with_value(struct ltn_histogram_s *ctx, uint32_t diffMs)
{
	if ((diffMs < ctx->minValMs) || (diffMs > ctx->maxValMs)) {
		ctx->bucketMissCount++;
		return -1;
	}

	struct ltn_histogram_bucket_s *bucket = ltn_histogram_bucket(ctx, diffMs);

	struct timeval now;
	gettimeofday(&now, NULL);
	bucket->lastUpdate = now; /* Implicit struct copy. */
	bucket->count++;
	ctx->totalCount++;

	return diffMs;
}

static __inline__ int ltn_histogram_interval_update(struct ltn_histogram_s *ctx)
{
	struct timeval now;
	gettimeofday(&now, NULL);

	uint32_t diffMs = ltn_timeval_subtract_ms(&now, &ctx->intervalLast);

	ctx->intervalLast = now; /* Implicit struct copy. */

	if ((diffMs < ctx->minValMs) || (diffMs > ctx->maxValMs)) {
		ctx->bucketMissCount++;
		return -1;
	}

	struct ltn_histogram_bucket_s *bucket = ltn_histogram_bucket(ctx, diffMs);
	bucket->lastUpdate = now; /* Implicit struct copy. */
	bucket->count++;
	ctx->totalCount++;

	return diffMs;
}

static __inline__ void ltn_histogram_interval_print_buf(char **buf, struct ltn_histogram_s *ctx, unsigned int seconds)
{
	unsigned int blen = 4096;

	*buf = NULL;

	char *p = (char *)calloc(1, blen);
	if (!p) {
		return;
	}

	if (seconds) {
		struct timeval now;
		gettimeofday(&now, NULL);

		uint32_t diffMs = ltn_timeval_subtract_ms(&now, &ctx->printLast);

		if (diffMs < (seconds * 1000))
			return;

		ctx->printLast = now; /* Implicit struct copy. */
	}

	sprintf(p + strlen(p), "Histogram '%s' (ms, count, last update time, pct)\n", ctx->name);

	__int128 bucketTotals = 0;
	uint64_t cnt = 0, measurements = 0;
	for (uint32_t i = 0; i < ctx->bucketCount; i++) {
		struct ltn_histogram_bucket_s *b = &ctx->buckets[i];
		if (!b->count)
			continue;

		/* Compute the relative percentage vs total */
		bucketTotals += b->count;
		double overallPCT = ((double)b->count / (double)ctx->totalCount) * 100.0;
		double rankedPCT = ((double)bucketTotals / (double)ctx->totalCount) * 100.0;

		char timestamp[128];
		sprintf(timestamp, "%s", ctime(&b->lastUpdate.tv_sec));
		timestamp[strlen(timestamp) - 1] = 0; /* Trim trailing CR */

		sprintf(p + strlen(p),
			"-> %5" PRIu64 " %'15" PRIu64 "  %s  %10.6f%%  %10.6f%%\n",
			ctx->minValMs + i,
			b->count,
			timestamp,
			overallPCT,
			rankedPCT);

		cnt++;
		measurements += b->count;

		if (strlen(p) > (blen - 516)) {
			blen += 4096;
			p = (char *)realloc(p, blen);
		}
	}

	if (ctx->bucketMissCount) {
		sprintf(p + strlen(p), "%" PRIu64 " out-of-range bucket misses\n", ctx->bucketMissCount);
	}

	sprintf(p + strlen(p), "%" PRIu64 " distinct buckets with %'" PRIu64 " total measurements, range: %" PRIu64 " -> %" PRIu64 " ms\n",
		cnt,
		measurements,
		ctx->minValMs, ctx->maxValMs);

	*buf = p;
}

static __inline__ void ltn_histogram_interval_print(int fd, struct ltn_histogram_s *ctx, unsigned int seconds)
{
	if (seconds) {
		struct timeval now;
		gettimeofday(&now, NULL);

		uint32_t diffMs = ltn_timeval_subtract_ms(&now, &ctx->printLast);

		if (diffMs < (seconds * 1000))
			return;

		ctx->printLast = now; /* Implicit struct copy. */
	}

	dprintf(fd, "Histogram '%s' (ms, count, last update time, pct)\n", ctx->name);

	__int128 bucketTotals = 0;
	uint64_t cnt = 0, measurements = 0;
	for (uint32_t i = 0; i < ctx->bucketCount; i++) {
		struct ltn_histogram_bucket_s *b = &ctx->buckets[i];
		if (!b->count)
			continue;

		/* Compute the relative percentage vs total */
		bucketTotals += b->count;
		double overallPCT = ((double)b->count / (double)ctx->totalCount) * 100.0;
		double rankedPCT = ((double)bucketTotals / (double)ctx->totalCount) * 100.0;

		char timestamp[128];
		sprintf(timestamp, "%s", ctime(&b->lastUpdate.tv_sec));
		timestamp[strlen(timestamp) - 1] = 0; /* Trim trailing CR */

		dprintf(fd,
			"-> %5" PRIu64 " %'15" PRIu64 "  %s  %10.6f%%  %10.6f%%\n",
			ctx->minValMs + i,
			b->count,
			timestamp,
			overallPCT,
			rankedPCT);

		cnt++;
		measurements += b->count;
	}

	if (ctx->bucketMissCount) {
		dprintf(fd, "%" PRIu64 " out-of-range bucket misses\n", ctx->bucketMissCount);
	}

	dprintf(fd, "%" PRIu64 " distinct buckets with %'" PRIu64 " total measurements, range: %" PRIu64 " -> %" PRIu64 " ms\n",
		cnt,
		measurements,
		ctx->minValMs, ctx->maxValMs);
}

static __inline__ void ltn_histogram_summary_print(int fd, struct ltn_histogram_s *ctx, unsigned int seconds, unsigned int bucketSizeMs)
{
	if (seconds) {
		struct timeval now;
		gettimeofday(&now, NULL);

		uint32_t diffMs = ltn_timeval_subtract_ms(&now, &ctx->printSummaryLast);

		if (diffMs < (seconds * 1000))
			return;

		ctx->printSummaryLast = now; /* Implicit struct copy. */
	}

	struct ltn_histogram_s *h;
	char name[256];
	sprintf(name, "%s - Summarized into buckets of %d ms", ctx->name, bucketSizeMs);
	ltn_histogram_alloc(&h, name, ctx->minValMs, ctx->maxValMs);

	/* Walk all of the buckets based on ms, grab the bucket and summary it into a new histogram with a new bucketsize */
	for (uint64_t i = ctx->minValMs; i < ctx->maxValMs; i += bucketSizeMs) {
		struct ltn_histogram_bucket_s *dst = ltn_histogram_bucket(h, i + bucketSizeMs);

		for (uint64_t j = 0; j < (bucketSizeMs - 1); j++) {
			struct ltn_histogram_bucket_s *src = ltn_histogram_bucket(ctx, i + j);
			if (!src)
				continue;

			dst->count += src->count;

			/* We want the latest update time in the summarized histogram. */
			if (_compareTime(&src->lastUpdate, &dst->lastUpdate) > 0)
				dst->lastUpdate = src->lastUpdate; /* Implicit struct copy. */
		}
	}
	ltn_histogram_interval_print(fd, h, seconds);
	ltn_histogram_free(h);
}


__inline__ static void ltn_histogram_cumulative_initialize(struct ltn_histogram_s *ctx)
{
	ctx->cumulativeMs = 0;
}

__inline__ static void ltn_histogram_cumulative_begin(struct ltn_histogram_s *ctx)
{
	gettimeofday(&ctx->cumulativeLast, 0);
}

__inline__ static uint64_t ltn_histogram_cumulative_end(struct ltn_histogram_s *ctx)
{
	struct timeval now;
	gettimeofday(&now, 0);

	uint64_t val = ltn_timeval_subtract_ms(&now, &ctx->cumulativeLast);
	ctx->cumulativeMs += val;

	return val;
}

__inline__ static uint64_t ltn_histogram_cumulative_finalize(struct ltn_histogram_s *ctx)
{
	/* Write ctx->cumulativeMs into the buckets. */
	if ((ctx->cumulativeMs < ctx->minValMs) || (ctx->cumulativeMs > ctx->maxValMs)) {
		ctx->bucketMissCount++;
	} else {
		struct ltn_histogram_bucket_s *bucket = ltn_histogram_bucket(ctx, ctx->cumulativeMs);
		gettimeofday(&bucket->lastUpdate, 0);
		bucket->count++;
		ctx->totalCount++;
	}

	return ctx->cumulativeMs;
}

__inline__ static void ltn_histogram_sample_begin(struct ltn_histogram_s *ctx)
{
	gettimeofday(&ctx->sampleLast, 0);
}

__inline__ static uint64_t ltn_histogram_sample_end(struct ltn_histogram_s *ctx)
{
	struct timeval now;
	gettimeofday(&now, 0);

	ctx->sampleMs = ltn_timeval_subtract_ms(&now, &ctx->sampleLast);

	/* Write ctx->sampleMs into the buckets. */
	if ((ctx->sampleMs < ctx->minValMs) || (ctx->sampleMs > ctx->maxValMs)) {
		ctx->bucketMissCount++;
	} else {
		struct ltn_histogram_bucket_s *bucket = ltn_histogram_bucket(ctx, ctx->sampleMs);
		gettimeofday(&bucket->lastUpdate, 0);
		bucket->count++;
		ctx->totalCount++;
	}

	return ctx->sampleMs;
}

#endif /* LTN_HISTOGRAM_H */

