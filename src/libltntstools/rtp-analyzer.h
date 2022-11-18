
#ifndef KL_RTP_ANALYZER_H
#define KL_RTP_ANALYZER_H

/**
 * @file        rtp.h
 * @author      Steven Toth <stoth@kernellabs.com>
 * @copyright   Copyright (c) 2017-2023 Kernel Labs Inc. All Rights Reserved.
 * @brief       A basic RTP analyzer, pulled from KL project into open source for the benefit of all mankind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h> 
#include <libltntstools/histogram.h> 

//
// please define based on your architecture.
// RTP_LITTLE_ENDIAN seems to work for OS X El Capitan
//
#define RTP_LITTLE_ENDIAN 1
struct rtp_hdr {
#if RTP_BIG_ENDIAN
    unsigned int version:2;   /* protocol version */
    unsigned int p:1;         /* padding flag */
    unsigned int x:1;         /* header extension flag */
    unsigned int cc:4;        /* CSRC count */
    unsigned int m:1;         /* marker bit */
    unsigned int pt:7;        /* payload type */
#elif RTP_LITTLE_ENDIAN
    unsigned int cc:4;        /* CSRC count */
    unsigned int x:1;         /* header extension flag */
    unsigned int p:1;         /* padding flag */
    unsigned int version:2;   /* protocol version */
    unsigned int pt:7;        /* payload type */
    unsigned int m:1;         /* marker bit */
#else
#error Define one of RTP_LITTLE_ENDIAN or RTP_BIG_ENDIAN
#endif

    unsigned int seq:16;      /* sequence number */
    u_int32_t ts;               /* timestamp */
    u_int32_t ssrc;             /* synchronization source */
} __attribute__((packed));

struct rtp_hdr_analyzer_s
{
	struct rtp_hdr last;
	int64_t totalPackets;
	int64_t totalFrames;
	int64_t discontinuityEvents;
    int64_t emptyPacketEvents;
	int64_t illegalPayloadTypeEvents;
	int64_t illegal2110TimestampMovementEvents;
	int64_t illegal2110TimestampStallEvents;
	int64_t illegalTSTimestampMovementEvents;
	int64_t illegalTSTimestampStallEvents;
    int64_t illegalTSCounterMovementEvents;

    /* Timestamp movement histogram */
    struct ltn_histogram_s *tsInterval; /* The TS field, look at the data and determine how the clock is moving */
    struct ltn_histogram_s *tsArrival;  /* How frequency are the frames arriving? */

	/* SMPTE2110-20 (Video) Specific */
};

void rtp_analyzer_init(struct rtp_hdr_analyzer_s *ctx);
void rtp_analyzer_free(struct rtp_hdr_analyzer_s *ctx);

/* Wide / zero the stats */
void rtp_analyzer_reset(struct rtp_hdr_analyzer_s *ctx);


/**
 * @brief       Push a RTP header into the framework, have it analyze various metrics.
 * @param[in]   struct rtp_hdr_analyzer_s *ctx - A previously allocated context, see rtp_analyzer_init().
 * @param[in]   const struct rtp_hdr *hdr - Header
 * @return      0 on success else < 0 if error
 */
int rtp_hdr_write(struct rtp_hdr_analyzer_s *ctx, const struct rtp_hdr *hdr);

int rtp_hdr_is_payload_type_valid(const struct rtp_hdr *hdr);
int rtp_hdr_is_continious(struct rtp_hdr_analyzer_s *ctx, const struct rtp_hdr *hdr);
void rtp_analyzer_report_dprintf(struct rtp_hdr_analyzer_s *ctx, int fd);
void rtp_analyzer_hdr_dprintf(const struct rtp_hdr *h, int fd);

#endif /* KL_RTP_ANALYZER_H */
