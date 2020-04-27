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

void ltntstools_clock_reset(struct ltntstools_clock_s *clk, int64_t ticks_per_second)
{
	memset(clk, 0, sizeof(*clk));
	clk->ticks_per_second = ticks_per_second;
}

void ltntstools_clock_establish_wallclock(struct ltntstools_clock_s *clk, int64_t time)
{
	gettimeofday(&clk->establishedWalltime, NULL);
	clk->establishedTime = time;
	clk->currentTime = time;
}

void ltntstools_clock_set_ticks(struct ltntstools_clock_s *clk, int64_t ticks)
{
	clk->currentTime = ticks;
}

void ltntstools_clock_add_ticks(struct ltntstools_clock_s *clk, int64_t ticks)
{
	clk->currentTime += ticks;
}

int64_t ltntstools_clock_get_drift_us(struct ltntstools_clock_s *clk)
{
	struct timeval ts, diff, diff2;
	gettimeofday(&ts, NULL);

	/* Establish how many usecs have passed since the established time. */
	_timeval_subtract(&diff, &ts, &clk->establishedWalltime);

	/* How many ticks have passed since we established time? as a timeval struct */
	double ticks = clk->currentTime - clk->establishedTime;

	struct timeval tickTime;
	tickTime.tv_sec = ticks / clk->ticks_per_second;
	double n = (ticks / (double)clk->ticks_per_second) * 1000000.0;
	tickTime.tv_usec = n;
	tickTime.tv_usec %= 1000000;

	_timeval_subtract(&diff2, &tickTime, &diff);
	return _timeval_to_us(&diff2);
}

int64_t ltntstools_clock_get_drift_ms(struct ltntstools_clock_s *clk)
{
	return ltntstools_clock_get_drift_us(clk) / 1000;
}
