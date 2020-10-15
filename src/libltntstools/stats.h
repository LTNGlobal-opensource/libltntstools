/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#ifndef _STATS_H
#define _STATS_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PID 8192
struct ltntstools_pid_statistics_s
{
	int enabled;
	uint64_t packetCount;
	uint64_t ccErrors;
	uint64_t teiErrors;

	uint8_t lastCC;

	/* Maintain a packets per second count, we can convert this into Mb/ps */
	time_t pps_last_update;
	uint32_t pps;
	uint32_t pps_window;
	double mbps; /* Updated once per second. */
};

struct ltntstools_stream_statistics_s
{
	struct ltntstools_pid_statistics_s pids[MAX_PID];
	uint64_t packetCount;
	uint64_t teiErrors;
	uint64_t ccErrors;

	/* Maintain a packets per second count, we can convert this into Mb/ps */
	time_t pps_last_update;
	uint32_t pps;
	uint32_t pps_window;
	double mbps; /* Updated once per second. */
};

int ltntstools_isCCInError(const uint8_t *pkt, uint8_t oldCC);
void ltntstools_pid_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *pkts, uint32_t packetCount);
void ltntstools_pid_stats_reset(struct ltntstools_stream_statistics_s *stream);

double   ltntstools_pid_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_pid_stats_stream_get_pps(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_pid_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream);
uint64_t ltntstools_pid_stats_stream_get_cc_errors(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_pid_stats_stream_padding_pct(struct ltntstools_stream_statistics_s *stream);

double   ltntstools_pid_stats_pid_get_mbps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
uint32_t ltntstools_pid_stats_pid_get_pps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
uint32_t ltntstools_pid_stats_pid_get_bps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
uint64_t ltntstools_pid_stats_pid_get_packet_count(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

#ifdef __cplusplus
};
#endif

#endif /* _STATS_H */
