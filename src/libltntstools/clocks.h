#ifndef _CLOCKS_H
#define _CLOCKS_H

/**
 * @file        clocks.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       Track walltime bs another timebase and compute drift deltas between the them.
 *              Useful when measuring how a stream of video is drifting or how much jitter the
 *              clocks have, compared to reality.
 */

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Walltime: time on the running host, measured in uS.
 * Timebase: A clock runnig at a rate of 27MHz, or 90Khz, or any other rate, for example.
 * Ticks: a unit of measure, generally related to the timebase
 * 
 * Walltime moves naturally on its own, it advanced.
 * The clock context and its timebase does not, it only moves when the caller
 * passes in new tick values, as additional increments or as an absolute value.
 * 
 */

/**
 * @brief API context related to walltime vs timebase clock monitoring.
 */
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

	int64_t clockWrapValue; /* Upper limited so we can compute wrapping clocks. */
};

/**
 * @brief       First call any user should make, it to initialize the context.
 *              Once initialized, the clock context can be used routinely with the other APIs.
 * @param[in]   struct ltntstools_clock_s *clk - user allocated storage for the clock context
 */
void ltntstools_clock_initialize(struct ltntstools_clock_s *clk);

/**
 * @brief       Establish a timebase of N ticks_per_second, for this clock context.
 *              For example:     90,000 FOR PTS/DTS
 *                           27,000,000 FOR PCR
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @param[in]   int64_t ticks_per_second - Eg. 27,000,000
 */
void ltntstools_clock_establish_timebase(struct ltntstools_clock_s *clk, int64_t ticks_per_second);

/**
 * @brief       Determine if the timebase has been established, via an earlier call to ltntstools_clock_establish_timebase()
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @return      Boolean. 1 on success else 0.
 */
int  ltntstools_clock_is_established_timebase(struct ltntstools_clock_s *clk);

/**
 * @brief       Once the timebase bas been established (ltntstools_clock_reset()),
 *              syncronize 'ticks' (and absolute measurement of time in the establish timebase,
 *              to walltime (measured in us).
 *              This measns we'll be able to measure drift, in the future.
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @param[in]   int64_t ticks - current measurement for the clock context (that will be aligned to walltime).
 */
void ltntstools_clock_establish_wallclock(struct ltntstools_clock_s *clk, int64_t ticks);

/**
 * @brief       Determine if the wallclock has been established, via an earlier call to ltntstools_clock_establish_wallclock()
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @return      Boolean. 1 on success else 0.
 */
int  ltntstools_clock_is_established_wallclock(struct ltntstools_clock_s *clk);

/**
 * @brief       Set the current timebase clock to an absolute value of 'ticks'.
 *              For example, after you've extracted a PTS from the PES, or a PCR clock from a frame,
 *              you should call this method to syncronize the timebase to walltime.
 *              In all cases, 'ticks' should be a positive value.
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @param[in]   int64_t ticks - absolute value
 */
void ltntstools_clock_set_ticks(struct ltntstools_clock_s *clk, int64_t ticks);

/**
 * @brief       Get the current timebase clock to an absolute value of 'ticks'.
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @return      int64_t ticks - current measurement from clock context.
 */
int64_t ltntstools_clock_get_ticks(struct ltntstools_clock_s *clk);

/**
 * @brief       Add N ticks to the existing clock context, positive or negative values are supported.
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @param[in]   int64_t ticks - relative value
 */
void ltntstools_clock_add_ticks(struct ltntstools_clock_s *clk, int64_t ticks);

/**
 * @brief       Query the drift between walltime and the timebase context, expressed in uS.
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @return      int64_t - ticks
 */
int64_t ltntstools_clock_get_drift_us(struct ltntstools_clock_s *clk);

/**
 * @brief       Query the drift between walltime and the timebase context, expressed in mS.
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @return      int64_t - ticks
 */
int64_t ltntstools_clock_get_drift_ms(struct ltntstools_clock_s *clk);

/**
 * @brief       Compute the delta (always expressed as positive) between two timebase values.
 *              For example, computing the difference between two tick values in this timebase,
 *              regardless of whether the clock will wrap or not.
 *              Supported for 27MHz and 90 kHz clocks.
 * @param[in]   struct ltntstools_clock_s *clk - context
 * @param[in]   int64_t ticks - timeA
 * @param[in]   int64_t ticks - timeB
 * @return      int64_t - ticks
 */
int64_t ltntstools_clock_compute_delta(struct ltntstools_clock_s *clk, int64_t ticksnow, int64_t ticksthen);

#ifdef __cplusplus
};
#endif

#endif /* _CLOCKS_H */


