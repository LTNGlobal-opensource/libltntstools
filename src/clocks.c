/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include "libltntstools/ltntstools.h"
static int64_t _timeval_to_ms(struct timeval *tv)
{
        return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}

static int64_t _timeval_to_us(struct timeval *tv)
{
        return (tv->tv_sec * 1000000) + tv->tv_usec;
}

static int64_t _timeval_to_pcr(struct timeval *tv)
{
		return ((tv->tv_sec * 1000000) + tv->tv_usec) * 27;
}

static int _timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y)
{
        /* Perform the carry for the later subtraction by updating y. */
        if (x->tv_usec < y->tv_usec) {
                int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
                y->tv_usec -= 1000000 * nsec;
                y->tv_sec += nsec;
        }
        if (x->tv_usec - y->tv_usec > 1000000) {
                int nsec = (x->tv_usec - y->tv_usec) / 1000000;
                y->tv_usec += 1000000 * nsec;
                y->tv_sec -= nsec;
        }

        /* Compute the time remaining to wait. tv_usec is certainly positive. */
        result->tv_sec = x->tv_sec - y->tv_sec;
        result->tv_usec = x->tv_usec - y->tv_usec;

        /* Return 1 if result is negative. */
        return x->tv_sec < y->tv_sec;
}

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

void ltntstools_clock_add_ticks(struct ltntstools_clock_s *clk, int64_t ticks)
{
	clk->currentTime_ticks += ticks;
}

int64_t ltntstools_clock_get_drift_us(struct ltntstools_clock_s *clk)
{
	struct timeval now, elapsedWT, diff2;
	gettimeofday(&now, NULL);

	/* Establish how many usecs have passed since the established time. */
	_timeval_subtract(&elapsedWT, &now, &clk->establishedWalltime);

	/* How many timebase ticks have passed since we established time? as a timeval struct */
	double ticks = clk->currentTime_ticks - clk->establishedTime_ticks;
#if 0
printf("clk->currentTime_ticks %" PRIi64 ",	clk->establishedTime_ticks %" PRIi64 "\n",
	clk->currentTime_ticks,
	clk->establishedTime_ticks);
#endif

	struct timeval tickTime;
	tickTime.tv_sec = ticks / clk->ticks_per_second;
	double n = (ticks / (double)clk->ticks_per_second) * 1000000.0;
	tickTime.tv_usec = n;
	tickTime.tv_usec %= 1000000;

	/* ticktime - walltime
	 *        5 - 4    = 1    (pcr was early)
	 *        4 - 6    = -2   (pcr was late)
	 */
	_timeval_subtract(&diff2, &tickTime, &elapsedWT);
	clk->drift_us = _timeval_to_us(&diff2);

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
