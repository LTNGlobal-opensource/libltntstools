#ifndef _SMOOTHER_PCR_H
#define _SMOOTHER_PCR_H

/**
 * @file        smoother-pcr.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       A basic framework for a bitrate smoother.
 * 
 * Usage example, demuxing and parsing Video frames on pid 0x31:
 * 
 *    void *myCB(void *userContext, unsigned char *buf, int byteCount)
 *    {
 *       // Do something with the newly smoothed packets
 *       // UDP transmit
 *    }
 * 
 *    void *hdl;
 *    lsmoother_pcr_alloc(&hdl, NULL, myCB, 30000, 7*188, 0x31, 20000000);
 * 
 *    while (1) {
 *      smoother_pcr_write(hdl, buf, 7);
 *    }
 * 
 *    smoother_pcr_free(hdl);
 */
#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       Callback function definition, where demuxed and parsed PES frames are delivered
 *              to your function.
 *              Receiving thread doesn't own the lifespan of the buffer,
 *              applications should send the output to the network inside
 *              this callback.
 *              DO NOT free the buffer when you're done with it, you don't own the lifespan of the buffer.
 */
typedef void (*smoother_pcr_output_callback)(void *userContext, unsigned char *buf, int byteCount);

/**
 * @brief       Allocate a framework context capable of smoothing MPEG-TS SPTS/MPTS multiplexes.
 * @param[in]   void **hdl - Handle / context for further use.
 * @param[in]   void *userContext - user private context, passed back to caller during callback.
 * @param[in]   smoother_pcr_output_callback cb - user supplied callback for output delivery
 * @param[in]   int itemsPerSecond - Approximate number of write calls you intend to make per second.
 * @param[in]   int itemLengthBytes - Eg. 7*188
 * @param[in]   uint64_t pcrPID - transport packet identifier that will be used to pace the output.
 * @param[in]   int inputMuxrate_bps - The expected input muxrate (if it was perfect), Eg. 20,000,000
 * @return      0 on success, else < 0.
 */
int  smoother_pcr_alloc(void **hdl, void *userContext, smoother_pcr_output_callback cb,
	int itemsPerSecond, int itemLengthBytes, uint16_t pcrPID, int inputMuxrate_bps);

/**
 * @brief       Free a previously allocate context.
 * @param[in]   void *hdl - Handle / context.
 */
void smoother_pcr_free(void *hdl);

/**
 * @brief       Write an entire MPTS into the framework.
 *              At a later point in time, the packets will be handed back to your
 *              callback in a smooth jitter free timeline.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   const uint8_t *pkts - one or more aligned transport packets
 * @param[in]   int lengthBytes - number of packets
 * @param[in]   struct timeval *ts - current timestamp.
 * @return      0 on success, else < 0
 */
int  smoother_pcr_write(void *hdl, const uint8_t *pkts, int lengthBytes, struct timeval *ts);

//int  smoother_pcr_expire(void *hdl, struct timeval *ts);

/* From is null then from default to 1 second ago.
 *  end is null then end details to now.
 */
//int64_t smoother_pcr_sumtotal_i64(void *hdl, uint32_t channel, struct timeval *from, struct timeval *to);

#ifdef __cplusplus
};
#endif

#endif /* _SMOOTHER_PCR_H */


