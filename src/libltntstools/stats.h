#ifndef _STATS_H
#define _STATS_H

/**
 * @file        stats.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       Parse and analyze MPEG-TS transport streams, collect and expose
 *              multiplex and pid specific statistics.
 * 
 * This frame also handles generic network streams (BYTESTREAM)
 * and ATSC3.0 A/324 Common TUnneling Protocol (CTP) streams.
 * You should expect to see some API differences for these type types of streams
 * and, clearly, statsistics will be limited in these use cases.
 * 
 * Usage example, demuxing and parsing Video frames on pid 0x31:
 * 
 *    struct ltntstools_stream_statistics_s myStats;
 *    ltntstools_pid_stats_reset(&myStats);
 * 
 *    while (1) {
 *      ltntstools_pid_stats_update(&myStats, pkts, 7);
 * 
 *      // Query CC issues on an ongoing basis.
 *      uint64_t count = ltntstools_pid_stats_stream_get_cc_errors(&myStats);
 *    }
 * 
 */
#include <time.h>
#include <inttypes.h>
#include <libltntstools/clocks.h>
#include <libltntstools/histogram.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PID 8192

#define LTNTSTOOLS_CC_REORDER_LIST_SIZE 32
#define LTNTSTOOLS_CC_REORDER_TRACKING_COUNT 16

struct ltntstools_cc_reorder_table_s
{
        uint16_t pid;                           /* We don't need this */
        uint32_t updateCount;                   /* Count up to REORDER_LIST_SIZE then stop */
        uint32_t writeIdx;                      /* Value 0..REORDER_LIST_SIZE */
        uint8_t  arr[LTNTSTOOLS_CC_REORDER_LIST_SIZE];
        uint32_t ccerror[LTNTSTOOLS_CC_REORDER_LIST_SIZE];
};

enum ltntstools_notification_event_e {
	EVENT_UNDEFINED = 0,

	/* PIDS */
	EVENT_UPDATE_PID_PUSI_DELIVERY_TIME, /**< pid.pusi_time_ms value changed. */
	EVENT_UPDATE_PID_PCR_EXCEEDS_40MS,   /**< pid.pcrExceeds40ms value changed. */
	EVENT_UPDATE_PID_PCR_WALLTIME,       /**< pid.lastPCRWalltimeDriftMs value changed. */

	/* STREAMS */
	EVENT_UPDATE_STREAM_CC_COUNT,        /**< stream.ccErrors changed. */
	EVENT_UPDATE_STREAM_TEI_COUNT,       /**< stream.teiErrors changed. */
	EVENT_UPDATE_STREAM_SCRAMBLED_COUNT, /**< stream.scrambledCount changed. */
	EVENT_UPDATE_STREAM_MBPS,            /**< stream.mbps changed. */
	EVENT_UPDATE_STREAM_IAT_HWM,         /**< stream.iat_hwm_us changed. */
};

/**
 * @brief       Callback function definition, where important events are passed to the called
 *              in realtime.
 */
typedef void (*ltntstools_notification_callback)(void *userContext, enum ltntstools_notification_event_e event,
	const struct ltntstools_stream_statistics_s *stats,
	const struct ltntstools_pid_statistics_s *pid);

/**
 * @brief A pid specific statistics container, contained within struct ltntstools_stream_statistics_s
 */
struct ltntstools_pid_statistics_s
{
	int      enabled;              /**< Boolean. is the pid available in the multiplex. */
	uint16_t pidNr;                /**< ISO13818 Pid Number */
	uint64_t packetCount;          /**< Number of packets processed. */
	uint64_t ccErrors;             /**< Number of continuity counter issues processed */
	uint64_t teiErrors;            /**< Number of transport error indicator issues processed */
	uint64_t scrambledCount;       /**< Number of times we've seen scrambled/encrypted packets */
	uint64_t pcrExceeds40ms;       /**< Number of times the PCR interval has exceeded 40ms */
	uint64_t prev_pcrExceeds40ms;  /**< Prior value of pcrExceeds40ms, updated every ltntstools_pid_stats_update() call */

	uint8_t  lastCC;               /**< Last CC value sobserved */

	time_t   pps_last_update;      /**< Maintain a packets per second count, we can convert this into Mb/ps */
	uint32_t pps;                  /**< Helper var for computing bitrate */
	uint32_t pps_window;           /**< Helper var for computing bitrate */
	double   mbps;                 /**< Updated once per second. */

	int hasPCR;                    /**< User specifically told is this PID will contain a PCR */
	int seenPCR;                   /**< Helper var to track PCR values seen, and skipped during startup for stability. */
#define ltntstools_CLOCK_PCR 0
#define ltntstools_CLOCK_PTS 1
#define ltntstools_CLOCK_DTS 2
	struct ltntstools_clock_s clocks[3]; /**< Three clocks potentially per pid. See ltntstools_CLOCK_PCR, ltntstools_CLOCK_PTS and ltntstools_CLOCK_DTS */

	struct ltn_histogram_s *pcrTickIntervals; /** < Measure tick differences between adjacent PCR clocks, track the deltas. */
	struct ltn_histogram_s *pcrWallDrift; /** < Measure PCR vs Walltime and look for clock drift */
	int64_t lastPCRWalltimeDriftMs;       /** amount of drift in ms, positive or minus, from walltime. */

	struct ltntstools_cc_reorder_table_s *reorderTable; /** Detect UDP packet reordering */
};

/**
 * @brief A larger statistics container, representing all pids in an entire SPTS/MPTS.
 * Structure is approximately 3MB, plus an additional 2x256KB for each PCR PID.
 * So a single SPTS mux needs 3.5MB of RAM. 
 */
struct ltntstools_stream_statistics_s
{
	struct ltntstools_pid_statistics_s pids[MAX_PID];
	uint64_t packetCount;          /**< Total number of packets processed. */
	uint64_t teiErrors;            /**< Total number of transport error indicator issues processed */
	uint64_t ccErrors;             /**< Total number of continuity counter issues processed */
	uint64_t scrambledCount;       /**< Total number of times we've seen scrambled/encrypted packets */
	uint64_t pcrExceeds40ms;       /**< Total number of times the PCR interval has exceeded 40ms */
	uint64_t prev_pcrExceeds40ms;  /**< Prior value of pcrExceeds40ms, updated every ltntstools_pid_stats_update() call */
	uint64_t reorderErrors;        /**< Total number of times we've seen out of order packets create CC errors */

	time_t pps_last_update;        /**< Maintain a packets per second count, we can convert this into Mb/ps */
	uint32_t pps;                  /**< Helper var for computing bitrate */
	uint32_t pps_window;           /**< Helper var for computing bitrate */
	double mbps;                   /**< Updated once per second. */

	uint16_t a324_sequence_number; /**< A/324 - Last seqeuence number observed. */

	time_t Bps_last_update;        /**< A/324 Maintain a packets per second count, we can convert this into Mb/ps */
	uint32_t Bps;                  /**< Helper var for computing bitrate */
	uint32_t Bps_window;           /**< Helper var for computing bitrate */
	double a324_mbps;              /**< Updated once per second. */
	double a324_bps;               /**< Updated once per second. */

	time_t last_cc_error;          /**< For any CC error on any pid */

	uint64_t notMultipleOfSevenError; /**< number of times ltntstools_pid_stats_update() was called with a packetCount !- 7 */
	time_t last_notMultipleOfSeven_error;    /**< last time notMultipleOfSevenError was incremented */

	int iat_lwm_us; /**< IAT low watermark (us), measurement of UDP receive interval */
	int iat_hwm_us; /**< IAT high watermark (us), measurement of UDP receive interval */
	int iat_cur_us; /**< IAT current measurement (us) */
	int iat_hwm_us_last_nsecond; /**< IAT high watermark (us), for the last Nsecond, measurement of UDP receive interval */
	int iat_hwm_us_last_nsecond_accumulator; /**< IAT high watermark (us), for the last Nsecond, measurement of UDP receive interval */
	time_t iat_hwm_us_last_nsecond_time; /**< time the per-second IAT measurement reports to. */
	struct timeval iat_last_frame; /**< Timestamp of last UDP frame for this entity. */

	struct ltn_histogram_s *packetIntervals;
};

/**
 * @brief       For a given packet, and a known previous continuity counter value, determine
 *              if pkt is sequentually continious, or not.
 * @param[in]   const uint8_t *pkt - A fully aligned single transport packet.
 * @param[in]   uint8_t oldCC - Previous CC value for this packet pid.
 * @return      Boolean.
 */
int ltntstools_isCCInError(const uint8_t *pkt, uint8_t oldCC);

/**
 * @brief       Write an entire MPTS into the framework, update the stream and pid statistics.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   const uint8_t *pkts - one or more aligned transport packets
 * @param[in]   uint32_t packetCount - number of packets
 */
void ltntstools_pid_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *pkts, uint32_t packetCount);

/**
 * @brief       Write a basic ascii pid report to the file descriptor;
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   int fd - file descriptor
 */
void ltntstools_pid_stats_dprintf(struct ltntstools_stream_statistics_s *stream, int fd);

/**
 * @brief       Reset all statistics.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 */
void ltntstools_pid_stats_reset(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Allocate a new stats object instance.
 * @param[out]  struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      0 - Success, else < 0 on error.
 */
int ltntstools_pid_stats_alloc(struct ltntstools_stream_statistics_s **stream);

/**
 * @brief       Free a previously allocated stats object.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 */
void ltntstools_pid_stats_free(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Allocate a new stats object instance, duplicate the contents of src into it.
 *              Caller responsible for freeing the previous dst object, if it was used;
 * @param[in]   struct ltntstools_stream_statistics_s *src - Handle / context.
 * @return      full deep object copy, or NULL
 */
struct ltntstools_stream_statistics_s * ltntstools_pid_stats_clone(struct ltntstools_stream_statistics_s *src);

/**
 * @brief       Query CTP stream bitrate in Mb/ps
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      double - bitrate
 */
double   ltntstools_ctp_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query BYTESTREAM stream bitrate in Mb/ps
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      double - bitrate
 */
double   ltntstools_bytestream_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream bitrate in Mb/ps
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      double - bitrate
 */
double   ltntstools_pid_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream, cumulative count of out of order UDP frame issues measured. (DISABLED)
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      double - bitrate
 */
uint64_t ltntstools_pid_stats_stream_get_reorder_errors(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - transport packets per second.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint32_t  - packets per second
 */
uint32_t ltntstools_pid_stats_stream_get_pps(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - transport bits per second.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint32_t  - bits per second
 */
uint32_t ltntstools_pid_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query CTP stream - transport bits per second.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint32_t  - bits per second
 */
uint32_t ltntstools_ctp_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query BYTESTREAM stream - transport bits per second.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint32_t  - bits per second
 */
uint32_t ltntstools_bytestream_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - CC error count since last ltntstools_pid_stats_reset()
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint64_t - count
 */
uint64_t ltntstools_pid_stats_stream_get_cc_errors(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - time of last cc error.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      time_t - datetime, zero if no cc errors detected
 */
time_t ltntstools_pid_stats_stream_get_cc_error_time(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - Number of times ltntstools_pid_stats_update() was called with packetCount != 7. 
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint64_t - count
 */
uint64_t ltntstools_pid_stats_stream_get_notmultipleofseven_errors(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - Time of last NotAMultipleofSeven error.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      time_t - datetime, zero if no cc errors detected
 */
time_t ltntstools_pid_stats_stream_get_notmultipleofseven_time(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - Transport error indicator count since last ltntstools_pid_stats_reset()
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint64_t - count
 */
uint64_t ltntstools_pid_stats_stream_get_tei_errors(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - Scrambled packets detected since last ltntstools_pid_stats_reset()
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint64_t - count
 */
uint64_t ltntstools_pid_stats_stream_get_scrambled_count(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - overall stream padding percentage for the entire mux.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      uint32_t - percent
 */
uint32_t ltntstools_pid_stats_stream_padding_pct(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT stream - Did any pids violate PCR transport timing windows?
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @return      Boolean.
 */
int      ltntstools_pid_stats_stream_did_violate_pcr_timing(struct ltntstools_stream_statistics_s *stream);

/**
 * @brief       Query TRANSPORT, bitrate in Mb/ps, specifically for input pid.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      double - bitrate
 */
double   ltntstools_pid_stats_pid_get_mbps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query TRANSPORT, packets per second (188), specifically for input pid.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      packets per second
 */
uint32_t ltntstools_pid_stats_pid_get_pps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query TRANSPORT, bits per second (188), specifically for input pid.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      bps
 */
uint32_t ltntstools_pid_stats_pid_get_bps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query TRANSPORT, packet count, specifically for input pid.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      packet count
 */
uint64_t ltntstools_pid_stats_pid_get_packet_count(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query TRANSPORT, last time the pid statistics were updated, specifically for input pid.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      time_t lastUpdate
 */
time_t   ltntstools_pid_stats_pid_get_last_update(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query TRANSPORT - Did input pid violate PCR transport timing windows?
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      Boolean.
 */
int ltntstools_pid_stats_pid_did_violate_pcr_timing(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query TRANSPORT - Inform framework that this pid contains a PCR and PCR clocks math should be performed.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 */
void ltntstools_pid_stats_pid_set_contains_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query TRANSPORT - Check if input PID is expected to have a PCR.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      Boolean.
 */
int ltntstools_pid_stats_pid_get_contains_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query the current PCR tick value, if this PID contains a PCR. Else, return 0.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @return      PCR tick value in a 27MHz clock.
 */
int64_t ltntstools_pid_stats_pid_get_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr);

/**
 * @brief       Query the current pid PCR vs walltime drift, assuming this PID contains a PCR. Else, return -1.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint16_t pidnr - pid
 * @param[out]  int64_t driftMs - Amount of drift ahead of walltime, or behind walltime, the PCR is
 * @return      0 - Success, else < 0 on error.
 */
int ltntstools_pid_stats_pid_get_pcr_walltime_driftms(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr, int64_t *driftMs);

/**
 * @brief       Write a CTP buffer into the stats layer.
 *              Limited but useful stats will be collected and exposed.
 *              ATSC3.0 A/324 stats wedged into this framework, better than nothing.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   const uint8_t *buf - CTP buffer of bytes
 * @param[in]   uint32_t lengthBytes - length of CTP buffer in bytes
 */
void ltntstools_ctp_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *buf, uint32_t lengthBytes);

/**
 * @brief       Write a generic BYTESTREAM buffer into the stats layer.
 *              Limited but useful stats will be collected and exposed.
 *              Use for SMPTE2110 for example.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   const uint8_t *buf - CTP buffer of bytes
 * @param[in]   uint32_t lengthBytes - length of CTP buffer in bytes
 */
void ltntstools_bytestream_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *buf, uint32_t lengthBytes);

/**
 * @brief       Get the IAT high watermark measured in 'us', of all update calls in the last 5 seconds.
 * @param[in]   struct ltntstools_stream_statistics_s *stream - Handle / context.
 * @param[in]   uint64_t lengthBytes - length of CTP buffer in bytes
 */
uint64_t ltntstools_pid_stats_stream_get_iat_hwm_us(struct ltntstools_stream_statistics_s *stream);

#ifdef __cplusplus
};
#endif

#endif /* _STATS_H */
