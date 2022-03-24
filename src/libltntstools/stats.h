/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#ifndef _STATS_H
#define _STATS_H

#include <time.h>
#include <inttypes.h>
#include <libltntstools/clocks.h>

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
	uint64_t scrambledCount;

	uint8_t lastCC;

	/* Maintain a packets per second count, we can convert this into Mb/ps */
	time_t pps_last_update;
	uint32_t pps;
	uint32_t pps_window;
	double mbps; /* Updated once per second. */

	int hasPCR;
	int seenPCR;
#define ltntstools_CLOCK_PCR 0
#define ltntstools_CLOCK_PTS 1
#define ltntstools_CLOCK_DTS 2
	struct ltntstools_clock_s clocks[3];
};

struct ltntstools_stream_statistics_s
{
	struct ltntstools_pid_statistics_s pids[MAX_PID];
	uint64_t packetCount;
	uint64_t teiErrors;
	uint64_t ccErrors;
	uint64_t scrambledCount;

	/* Maintain a packets per second count, we can convert this into Mb/ps */
	time_t pps_last_update;
	uint32_t pps;
	uint32_t pps_window;
	double mbps; /* Updated once per second. */

	/* A/324 specific */
	uint16_t a324_sequence_number;

	/* A/324 Maintain a packets per second count, we can convert this into Mb/ps */
	time_t Bps_last_update;
	uint32_t Bps;
	uint32_t Bps_window;
	double a324_mbps;
	double a324_bps;
};

int ltntstools_isCCInError(const uint8_t *pkt, uint8_t oldCC);
void ltntstools_pid_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *pkts, uint32_t packetCount);
void ltntstools_pid_stats_reset(struct ltntstools_stream_statistics_s *stream);

double   ltntstools_ctp_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream);
double   ltntstools_bytestream_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream);
double   ltntstools_pid_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_pid_stats_stream_get_pps(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_pid_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_ctp_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_bytestream_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream);
uint64_t ltntstools_pid_stats_stream_get_cc_errors(struct ltntstools_stream_statistics_s *stream);
uint64_t ltntstools_pid_stats_stream_get_tei_errors(struct ltntstools_stream_statistics_s *stream);
uint64_t ltntstools_pid_stats_stream_get_scrambled_count(struct ltntstools_stream_statistics_s *stream);
uint32_t ltntstools_pid_stats_stream_padding_pct(struct ltntstools_stream_statistics_s *stream);

double   ltntstools_pid_stats_pid_get_mbps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
uint32_t ltntstools_pid_stats_pid_get_pps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
uint32_t ltntstools_pid_stats_pid_get_bps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
uint64_t ltntstools_pid_stats_pid_get_packet_count(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
time_t   ltntstools_pid_stats_pid_get_last_update(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

void ltntstools_pid_stats_pid_set_contains_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);
int ltntstools_pid_stats_pid_get_contains_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/* A324 stats wedged into this framework, better than nothing. */
void ltntstools_ctp_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *buf, uint32_t lengthBytes);
void ltntstools_bytestream_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *buf, uint32_t lengthBytes);

#ifdef __cplusplus
};
#endif

#endif /* _STATS_H */
