#ifndef _CLOCKS_H
#define _CLOCKS_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ltntstools_clock_s
{
	int64_t ticks_per_second;

	int64_t currentTime;

	struct timeval establishedWalltime; /* Walltime when we establish the 'establishedTime' field value. */
	int64_t establishedTime;
};

void ltntstools_clock_reset(struct ltntstools_clock_s *clk, int64_t ticks_per_second);
void ltntstools_clock_establish_wallclock(struct ltntstools_clock_s *clk, int64_t time);
void ltntstools_clock_set_ticks(struct ltntstools_clock_s *clk, int64_t ticks);
void ltntstools_clock_add_ticks(struct ltntstools_clock_s *clk, int64_t ticks);
int64_t ltntstools_clock_get_drift_us(struct ltntstools_clock_s *clk);
int64_t ltntstools_clock_get_drift_ms(struct ltntstools_clock_s *clk);

#ifdef __cplusplus
};
#endif

#endif /* _CLOCKS_H */


