#ifndef _CLOCKS_H
#define _CLOCKS_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ltntstools_clock_s
{
	int establishedTimebase;
	int establishedWT;

	int64_t ticks_per_second;

	int64_t currentTime_ticks;
	int64_t establishedTime_ticks;
	struct timeval establishedWalltime; /* Walltime when we establish the 'establishedTime' field value. */

	int64_t drift_us;
	int64_t drift_us_lwm;
	int64_t drift_us_hwm;
	int64_t drift_us_max;
};

/* First call any user should make, it to initialize the context. */
void ltntstools_clock_initialize(struct ltntstools_clock_s *clk);

/* Erase the context.
 * Establish a timebase of N ticks_per_second.
 * For example:     90,000 FOR PTS/DTS
 *              27,000,000 FOR PCR
 */
void ltntstools_clock_establish_timebase(struct ltntstools_clock_s *clk, int64_t ticks_per_second);
int  ltntstools_clock_is_established_timebase(struct ltntstools_clock_s *clk);

/* One the timebase bas been established (ltntstools_clock_reset()),
 * syncronize 'ticks' (and absolute measurement of time int he establish timebase,
 * to walltime (measured in us).
 * 
 * This measns we'll be able to measure drift, in the future.
 */
void ltntstools_clock_establish_wallclock(struct ltntstools_clock_s *clk, int64_t ticks);
int  ltntstools_clock_is_established_wallclock(struct ltntstools_clock_s *clk);

/* Set the current time (in timebase) to 'ticks'.
 * For example, after you've extracted a PTS from the PES, or a PCR clock from a frame,
 * you should call this method to syncronize the timebase to walltime.
 * In all cases, 'ticks' should be a positive value.
 */
void ltntstools_clock_set_ticks(struct ltntstools_clock_s *clk, int64_t ticks);

int64_t ltntstools_clock_get_ticks(struct ltntstools_clock_s *clk);

/* While normally _set_ticks is used to syncronize the timebase, you may wish
 * to closely control a clock and add N ticks to the timebase.
 * You may add a positive or negative value to bend the timebase value, if desired.
 */
void ltntstools_clock_add_ticks(struct ltntstools_clock_s *clk, int64_t ticks);

/* Query the */
int64_t ltntstools_clock_get_drift_us(struct ltntstools_clock_s *clk);
int64_t ltntstools_clock_get_drift_ms(struct ltntstools_clock_s *clk);

#ifdef __cplusplus
};
#endif

#endif /* _CLOCKS_H */


