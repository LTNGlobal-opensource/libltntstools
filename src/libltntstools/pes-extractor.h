#ifndef PES_EXTRACTOR_H
#define PES_EXTRACTOR_H

/**
 * @file        pes-extractor.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       Parse and demux MPEG transport streams and produce fully formed PES
 *              structures for analysis other work. Capable of parsing fixed length PES
 *              packets or variable length packets (larger than 65536 bytes). Operationally,
 *              this framework will never produce a callback with a mangled PES. Inorder to
 *              accomplish this, any packet loss (CC errors) on the chosen pid are tracked
 *              and partially corrupted streams discarded.
 * 
 * Usage example, demuxing and parsing Video frames on pid 0x31:
 *
 *    void myCB(void *userContext, struct ltn_pes_packet_s *pes)
 *    {
 *      ltn_pes_packet_dump(pes, "");
 *      ltn_pes_packet_free(pes);
 *    }
 *
 *    void *hdl;
 *    ltntstools_pes_extractor_alloc(&hdl, 0x31, 0xe0, myCB, NULL, -1, -1);
 *    ltntstools_pes_extractor_set_skip_data(hdl, 1); // Skip payload data
 * 
 *    while (1) {
 *      ltntstools_pes_extractor_write(hdl, buf, 7);
 *    }
 * 
 *    ltntstools_pes_extractor_free(hdl);
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       Callback function definition, where demuxed and parsed PES frames are delivered
 *              to your function. You, the developer, own the lifespan of the 'pes' object.
 *              Make sure you call ltn_pes_packet_free(pes) when you're done with it, else leak.
 */
typedef void (*pes_extractor_callback)(void *userContext, struct ltn_pes_packet_s *pes);

/**
 * @brief       Allocate a framework context capable of demuxing and parsing PES streams.
 * @param[out]  void **hdl - Handle / context for further use.
 * @param[in]   uint16_t pid - MPEG TS transport PID to be de-muxed
 * @param[in]   uint8_t streamId - PES StreamID (Eg. 0xc0 for audio0, 0xe0 for video0)
 * @param[in]   pes_extractor_callback cb - user supplied callback for PES frame delivery
 * @param[in]   void *userContext - user private context, passed back to caller during callback.
 * @param[in]   int buffer_min - Initial size of ring buffer for processing/detecting pes, eagerly allocated up front. Set to -1 to use the default of 2 * 1024
 * @param[in]   int buffer_max - Maximum size of ring buffer, grown into lazily as needed. Set to -1 to use the default of 4 * 1048576
 * @return      0 on success, else < 0. Fails if hdl is NULL, or if
 *              buffer_min or buffer_max is negative and not exactly -1
 *              (the only valid "use default" sentinel).
 */
int ltntstools_pes_extractor_alloc(void **hdl, uint16_t pid, uint8_t streamId, pes_extractor_callback cb, void *userContext, int buffer_min, int buffer_max);

/**
 * @brief       Free a previously allocate context. Safe to call with hdl == NULL (no-op).
 * @param[in]   void *hdl - Handle / context.
 */
void ltntstools_pes_extractor_free(void *hdl);

/**
 * @brief       Write an entire MPTS into the framework, pid filtering and demux the stream.
 *              Once an entire PES has been parsed, the caller is handed the PES structure via
 *              the callback. Its the users responsibiliy to manage the lifetime of the callback
 *              pes struct.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   const uint8_t *pkts - one or more aligned transport packets
 * @param[in]   int packetCount - number of packets
 * @return      number of packets in this batch that matched the configured
 *              pid (may be zero, eg. none of the packets belonged to this
 *              pid), -1 if the call was rejected because the context is
 *              closing down, or -2 if hdl or pkts is invalid (eg. NULL).
 */
ssize_t ltntstools_pes_extractor_write(void *hdl, const uint8_t *pkts, int packetCount);

/**
 * @brief       Control whether the PES payload data is attached to the PES struct during demuxing.
 *              By default it is (skip is off). This is for performance reasons, its heavier to
 *              add data to the pes (that without).
 *              Not all use cases need the PES data. If you don't want it, skip it via this call.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   int tf - Boolean. 1) skip/don't add data 0) add data (default)
 * @return      0 on success, -1 if hdl is NULL.
 */
int ltntstools_pes_extractor_set_skip_data(void *hdl, int tf);

/**
 * @brief       Ensure that the PES callbacks are always delivered ascending time order.
 *              The framework will cache a number of PES frames, then feed the callback the oldest
 *              frame based on assessing the PTS value from a list of (typically) 10 cached items.
 *              This obviously creates latency in the framework.
 *              Call this to enable the feature if needed, BEFORE the first _write call.
 *              Don't attempt to disable it once it's enabled.
 *              This feature is NOT enabled by default.
 *              This is helpful if you want to assess metadata attached to video frames which could
 *              NOT be in temporal order.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   int tf - Boolean. 1) enable ordered (PTS-sorted) output 0) disable (default)
 * @return      0 on success, -1 if hdl is NULL.
 */
int ltntstools_pes_extractor_set_ordered_output(void *hdl, int tf);

/**
 * @brief       Identify which pid carries the PCR for this stream, so PES's can be tagged with it.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   uint16_t pcrpidnr - PCR-bearing transport pid.
 * @return      0 on success, -1 if hdl is NULL.
 */
int ltntstools_pes_extractor_set_pcr_pid(void *hdl, uint16_t pcrpidnr);

#ifdef __cplusplus
};
#endif

#endif /* PES_EXTRACTOR_H */
