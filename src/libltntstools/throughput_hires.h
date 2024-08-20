#ifndef _THROUGHPUT_HIRES_H
#define _THROUGHPUT_HIRES_H

/**
 * @file        throughput-hirez.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020 LTN Global, Inc. All Rights Reserved.
 * @brief       A hiresolution scheme for tracking summable items over a usec accurate time window.
 *              The caller "categorizes" values into channels, Eg. PID, or sensor id, and
 *              the session can contain multiple channels for a given time period.
 * 
 *              Use this framework if you want a truly accurate calculation of 'things per second'.
 *
  * Typical usage for monitoring transport stream bitrates:
 * 
 * void *hdl;
 * throughput_hires_alloc(&hdl, 1000);
 * for each UDP transport frame {
 *   throughput_hires_write_i64(hdl, 0, 7 * 188 * 8, NULL;
 * }
 * 
 * In some other thread or time, query the bitrate:
 *    int64_t bps = throughput_hires_sumtotal_i64(hdl, 0, NULL, NULL);
 * 
 * 
 */
#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       Allocate a framework context capable of accurate measurements over time.
 * @param[out]  void **hdl - Handle / context for further use.
 * @param[in]   int itemsPerSecond - estimated number of items per second you want to measyre. For 
 *                                   example 13,000 UDP frames per second, or 30,000 transport packets
 *                                   per second. This is really a measure of how many times per second you
 *                                   anticipate calling throughput_hires_write_i64().
 * @return      0 on success, else < 0.
 */
int  throughput_hires_alloc(void **hdl, int itemsPerSecond);

/**
 * @brief       Write a in64_t into a data channel for later aggregation and summary reporting.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   uint32_t channel - items can be group into channels, categories. If you're not sure use value 0.
 * @param[in]   int64_t value - A value, int64, that will be sumed over a time-period to provide the answer.
 *                              An example of this might be 7 * 188 * 8, representing a Transport stream bitrate.
 * @param[in]   struct timeval *ts - time of event, or, if NULL the framework assumed walltime.
 */
void throughput_hires_write_i64(void *hdl, uint32_t channel, int64_t value, struct timeval *ts);

/**
 * @brief       Remove any int64_t items, for any channel, older than time ts. If you don't do this
 *              the items will accumulate endlessly. Typically, if I'm measuring transport stream
 *              throughput, I'll call this API after a few seconds to keep the internal lists
 *              small and less resource wasteful.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   struct timeval *ts - time of event, or, if NULL to represent 2 seconds ago.
 * @return      0 on success, else < 0.
 */
int  throughput_hires_expire(void *hdl, struct timeval *ts);

/**
 * @brief       Sum up, over a time window, the values of all the int64_t for a given channel.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   uint32_t channel - items can be group into channels, categories. If you're not sure use value 0.
 * @param[in]   struct timeval *from - From is null then from default to 1 second ago.
 * @param[in]   struct timeval *to - end is null then end details to now.
 * @param[in]   struct timeval *ts - time of event, or, if NULL the framework assumed walltime.
 */
int64_t throughput_hires_sumtotal_i64(void *hdl, uint32_t channel, struct timeval *from, struct timeval *to);

/**
 * @brief       Compute a min, max, average, the values of all the int64_t for a given channel.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   uint32_t channel - items can be group into channels, categories. If you're not sure use value 0.
 * @param[in]   struct timeval *from - From is null then from default to 1 second ago.
 * @param[in]   struct timeval *to - end is null then end details to now.
 * @param[in]   struct timeval *ts - time of event, or, if NULL the framework assumed walltime.
 * @param[out]  int64_t *min - minimum value 
 * @param[out]  int64_t *max - maximum value
 * @param[out]  int64_t *avg - average value
 */
int throughput_hires_minmaxavg_i64(void *hdl, uint32_t channel, struct timeval *from, struct timeval *to,
    int64_t *vmin,
    int64_t *vmax,
    int64_t *vavg);

/**
 * @brief       Free a previously allocate packet, and any attached payload
 * @param[in]   void *hdl - object
 */
void throughput_hires_free(void *hdl);

#ifdef __cplusplus
};
#endif

#endif /* _THROUGHPUT_HIRES_H */


