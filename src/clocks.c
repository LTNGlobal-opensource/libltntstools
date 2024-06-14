/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include "libltntstools/ltntstools.h"

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
	int64_t ticks = clk->currentTime_ticks - clk->establishedTime_ticks;
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
