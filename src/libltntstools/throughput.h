#ifndef _THROUGHPUT_H
#define _THROUGHPUT_H

/**
 * @file        throughput.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020 LTN Global, Inc. All Rights Reserved.
 * @brief       A computationally less expensive and thus less accurate version
 *              of throughput_hires.h Broadly speaking, things are measured over
 *              a one second period (with some slop), but it's good and close enough
 *              for many use cases with little or no processing overhead.
 */
#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ltntstools_throughput_s
{
	uint64_t byteCount;

	/* Maintain a bytes per second count, we can convert this into Mb/ps */
	time_t   Bps_last_update;
	uint32_t Bps;
	uint32_t Bps_window;

	double   mbps; /* Updated once per second. */
	double   mBps; /* Updated once per second. */
};

void     ltntstools_throughput_write_value(struct ltntstools_throughput_s *stream, int value);
void     ltntstools_throughput_write(struct ltntstools_throughput_s *stream, const uint8_t *buf, uint32_t byteCount);
void     ltntstools_throughput_reset(struct ltntstools_throughput_s *stream);
double   ltntstools_throughput_get_mbps(struct ltntstools_throughput_s *stream);
uint32_t ltntstools_throughput_get_bps(struct ltntstools_throughput_s *stream);
uint32_t ltntstools_throughput_get_value(struct ltntstools_throughput_s *stream);

#ifdef __cplusplus
};
#endif

#endif /* _THROUGHPUT_H */


