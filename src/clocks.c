/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include "libltntstools/ltntstools.h"

#define LOCAL_DEBUG 0

void ltntstools_clock_initialize(struct ltntstools_clock_s *clk)
{
	memset(clk, 0, sizeof(*clk));
}

int ltntstools_clock_is_established_timebase(struct ltntstools_clock_s *clk)
{
	return clk->establishedTimebase;
}

void ltntstools_clock_establish_timebase(struct ltntstools_clock_s *clk, int64_t ticks_per_second)
{
	clk->establishedTimebase = 1;
	clk->ticks_per_second = ticks_per_second;
	clk->establishedTime_ticks = 0;
	clk->currentTime_ticks = 0;

	if (clk->ticks_per_second == 90000)
		clk->clockWrapValue = MAX_PTS_VALUE;
	else
	if (clk->ticks_per_second == 27000000)
		clk->clockWrapValue = MAX_SCR_VALUE;
	else
		/* RTMP specifies that two timestamps with a uint32 delta of more than 2^31-1 ms
		 * are regressing, not wrapping, so this (with the check in _compute_delta) is
		 * only correct when timestamps are monotonic (which they usually are) */
		clk->clockWrapValue = (INT64_C(1) << 32);
}

int ltntstools_clock_is_established_wallclock(struct ltntstools_clock_s *clk)
{
	return clk->establishedWT;
}

void ltntstools_clock_establish_wallclock(struct ltntstools_clock_s *clk, int64_t ticks)
{
	clk->establishedWT = 1;
	gettimeofday(&clk->establishedWalltime, NULL);
	clk->establishedTime_ticks = ticks;
	clk->currentTime_ticks = ticks;
}

void ltntstools_clock_set_ticks(struct ltntstools_clock_s *clk, int64_t ticks)
{
	/* If the new tick value jumps the clock backwards by more than 50%, assume it naturally wrapped */
	if (ticks < (clk->currentTime_ticks / 50)) {
		clk->clockWrapOccurences++;
	}
	clk->currentTime_ticks = ticks;
}

int64_t ltntstools_clock_get_ticks(struct ltntstools_clock_s *clk)
{
	return clk->currentTime_ticks;
}

void ltntstools_clock_add_ticks(struct ltntstools_clock_s *clk, int64_t ticks)
{
	clk->currentTime_ticks += ticks;
}

int64_t ltntstools_clock_get_drift_us(struct ltntstools_clock_s *clk)
{
	/* Can't calculate drift when walltime has not been established yet */
	if (!clk->establishedWT)
		return 0;

	struct timeval now;
	gettimeofday(&now, NULL);

	/* Establish how many usecs have passed since the established time. */
	int64_t elapsedWT = ltn_timeval_subtract_us(&now, &clk->establishedWalltime);

	/* How many timebase ticks have passed since we established time? */
	int64_t ticks = (clk->clockWrapOccurences * clk->clockWrapValue) + (clk->currentTime_ticks - clk->establishedTime_ticks);
#if 0
printf("clk->currentTime_ticks %" PRIi64 ",	clk->establishedTime_ticks %" PRIi64 "\n",
	clk->currentTime_ticks,
	clk->establishedTime_ticks);
#endif

	/* ticktime in usecs */
	int64_t tickTime = ticks * 1000000 / clk->ticks_per_second;

	/* ticktime - walltime
	 *        5 - 4    = 1    (pcr was early)
	 *        4 - 6    = -2   (pcr was late)
	 */
	clk->drift_us = tickTime - elapsedWT;

	if (clk->drift_us > clk->drift_us_hwm)
		clk->drift_us_hwm = clk->drift_us;
	if (clk->drift_us <= clk->drift_us_lwm)
		clk->drift_us_lwm = clk->drift_us;

	clk->drift_us_max = clk->drift_us_hwm - clk->drift_us_lwm;

	return clk->drift_us;
}

int64_t ltntstools_clock_get_drift_ms(struct ltntstools_clock_s *clk)
{
	return ltntstools_clock_get_drift_us(clk) / 1000;
}

int64_t ltntstools_clock_compute_delta(struct ltntstools_clock_s *clk, int64_t ticksnow, int64_t ticksthen)
{
	/* We have to be able to deal with clock wrapping. */
	if (ticksnow >= ticksthen) {
		return ticksnow - ticksthen;
	}

	return (clk->clockWrapValue - ticksthen) + ticksnow;
}

/* JITTER_MAX: backward tolerance before clamping (optional). 0 = strict monotonic */
#define JITTER_MAX (90000/2)

/* Signed minimal delta on the 33-bit ring: result in (-2^32, +2^32] */
static inline int64_t pts33_signed_delta(struct ltntstools_corrected_clock_s *ctx, uint64_t curr, uint64_t prev)
{
    uint64_t a = curr & ctx->clk_mask;
	uint64_t b = prev & ctx->clk_mask;
    uint64_t diff = (a - b) & ctx->clk_mask;

	if (diff & ctx->clk_half) {
		return (int64_t)diff - (int64_t)ctx->clk_mod;
	} else {
		return (int64_t)diff;
	}
}

int ltntstools_corrected_clock_init(struct ltntstools_corrected_clock_s *ctx, unsigned int hz)
{
	if (hz == 90000) {
		ctx->clk_bits    = 33;
    	ctx->clkMaxTicks = (1ULL << (ctx->clk_bits));     /* 0x1ffffffff */
		ctx->clk_mod     = (1ULL << (ctx->clk_bits));     /* 0x1ffffffff */
		ctx->clk_mask    = ctx->clk_mod - 1;              /* 0x1fffffffe */
		ctx->clk_half    = (1ULL << (ctx->clk_bits - 1)); /* 0x0ffffffff */
	} else {
		/* No PCR support yet */
		return -1;
	}

    ctx->initialized = 1;
    ctx->lastTickValue = 0;
    ctx->unwrapped = 0;
    ctx->correctedClk = 0;

	return 0; /* Success */
}

/* Return an error if we think something unusual happened to the clock.
 * zero on success else < 0
 */
int ltntstools_corrected_clock_update(struct ltntstools_corrected_clock_s *ctx, int64_t ticks)
{
	if (!ctx || !ctx->initialized) {
		return -1; /* Failure */
	}

	int ret = 0; /* Success */

#if LOCAL_DEBUG
	printf("%s(%13" PRId64 ") ltv %13" PRId64 "\n", __func__, ticks, ctx->lastTickValue);
#endif
    uint64_t p33 = ((uint64_t)ticks) & ctx->clk_mask;

    if (ctx->initialized == 0) {
        ctx->initialized = 1;
        ctx->lastTickValue = p33;
        ctx->unwrapped = (int64_t)p33;
        ctx->correctedClk = (uint64_t)ctx->unwrapped;
        return ret;
    }

    int64_t d = pts33_signed_delta(ctx, p33, ctx->lastTickValue);
	//printf("%" PRIi64 "\n", d);

    /* Extend into 64-bit (can go slightly backward with B-frames) */
    ctx->unwrapped += d;

    /* Monotonic clamp (optionally allow small backward wiggle) */
	if (ctx->unwrapped < 0) {
#if LOCAL_DEBUG
		printf("Massive clock correction last ticks %13" PRId64 " new ticks %13" PRId64 ", corrected %13" PRId64 " unwrapped %13" PRId64 "\n",
			ctx->lastTickValue, p33,
			(int64_t)ctx->correctedClk, ctx->unwrapped);
#endif
		/* Effeective reset of the clocks */
		ctx->correctedClk = p33;
		ctx->unwrapped = p33;
		ret = -1;
	} else
    if ((int64_t)ctx->correctedClk - ctx->unwrapped > (int64_t)JITTER_MAX) {
        /* Large backward jump treat as reordering glitch: hold */
		//int64_t x = (int64_t)ctx->correctedClk - ctx->unwrapped;
    } else {
        if ((uint64_t)ctx->unwrapped > ctx->correctedClk) {
            ctx->correctedClk = (uint64_t)ctx->unwrapped;
		}
        /* within tolerance, keep previous */
    }
    ctx->lastTickValue = p33;

    return ret;
}

uint64_t ltntstools_corrected_clock_unwrapped(const struct ltntstools_corrected_clock_s *ctx)
{
	if (!ctx || !ctx->initialized) {
		return 0;
	}

    return (uint64_t)ctx->unwrapped;
}
