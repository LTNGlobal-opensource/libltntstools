#ifndef _SMOOTHER_RTP_H
#define _SMOOTHER_RTP_H

/**
 * @file        smoother-rtp.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2023 LTN Global,Inc. All Rights Reserved.
 * @brief       A basic framework for a bitrate smoother, and a useful mechanism
 *              for extracting a RTP clock value for each and every RTP frame.
 * 
 * TBD TBD TBD TBD
 * Usage example, demuxing and parsing Video frames on pid 0x31:
 * 
 *    int myCB(void *userContext, unsigned char *buf, int byteCount, struct ltntstools_pcr_position_s *array, int arrayLength)
 *    {
 *       // Do something with the newly smoothed packets, or the PCR values for each and every packet.
 *       // UDP transmit
 *    }
 * 
 *    void *hdl;
 *    lsmoother_rtp_alloc(&hdl, NULL, myCB, 30000, 12+(7*188), 100);
 * 
 *    while (1) {
 *      smoother_rtp_write(hdl, buf, lengthBytes, struct timeval *ts - or NULL);
 *    }
 * 
 *    smoother_rtp_free(hdl);
 */
#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ltntstools_rtp_position_s;

/**
 * @brief       Callback function definition, where RTP frames are delivered to your function.
 *              Receiving thread doesn't own the lifespan of the buffer,
 *              applications SHOULD send the output to the network inside this callback to preserve
 *              proper timing.
 *              DO NOT free the buffer or the array when you're done with it, you don't
 *              own the lifespan of the buffer.
 */
typedef int (*smoother_rtp_output_callback)(void *userContext, const unsigned char *buf, int byteCount);

/**
 * @brief       Allocate a framework context capable of smoothing MPEG-TS SPTS/MPTS multiplexes.
 * @param[in]   void **hdl - Handle / context for further use.
 * @param[in]   void *userContext - user private context, passed back to caller during callback.
 * @param[in]   smoother_rtp_output_callback cb - user supplied callback for output delivery
 * @param[in]   int itemsPerSecond - Approximate number of write calls you intend to make per second.
 * @param[in]   int itemLengthBytes - Minimum of 12+(7*188)
 * @param[in]   int latencyMS - The expected latency you want to project for jitter.
 * @return      0 on success, else < 0.
 */
int  smoother_rtp_alloc(void **hdl, void *userContext, smoother_rtp_output_callback cb,
	int itemsPerSecond, int itemLengthBytes, int latencyMS);

/**
 * @brief       Free a previously allocate context.
 * @param[in]   void *hdl - Handle / context.
 */
void smoother_rtp_free(void *hdl);

/**
 * @brief       Write one or more RTP streams into the framework.
 *              Fragments are acceptable, you don't need to write entire frames.
 *              At a later point in time, the packets will be handed back to your
 *              callback in a smooth jitter free timeline.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   const uint8_t *pkts - one or more aligned transport packets
 * @param[in]   int lengthBytes - number of bytes
 * @param[in]   struct timeval *ts - current timestamp.
 * @return      0 on success, else < 0
 */
int  smoother_rtp_write(void *hdl, const uint8_t *pkts, int lengthBytes, struct timeval *ts);

/**
 * @brief       Return the number of bytes held in the smoother queue.
 * @param[in]   void *hdl - Handle / context.
 * @return      >= 0 on success, else < 0 on error
 */
int64_t smoother_rtp_get_size(void *hdl);

/**
 * @brief       Delete all queued content, reset clocks, used when rewinding files.
 * @param[in]   void *hdl - Handle / context.
 */
void smoother_rtp_reset(void *hdl);

//int  smoother_rtp_expire(void *hdl, struct timeval *ts);

/* From is null then from default to 1 second ago.
 *  end is null then end details to now.
 */
//int64_t smoother_rtp_sumtotal_i64(void *hdl, uint32_t channel, struct timeval *from, struct timeval *to);

#ifdef __cplusplus
};
#endif

#endif /* _SMOOTHER_RTP_H */


