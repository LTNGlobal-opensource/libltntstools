#ifndef DEMUX_H
#define DEMUX_H

/**
 * @file        demux.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2026 LTN Global,Inc. All Rights Reserved.
 * @brief       Demultiplex a mpegts transport stream. Use callbacks to return various objects.
 *              Experimental. Work in progress. Do not use.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libltntstools/ltntstools.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       Callback function definition, where demuxed and parsed PES frames are delivered
 *              to your function. Caller DOES NOT OWN the lifespan of the returned object. If caller
 *              needs to preserve it, then duplicate it via ltn_pes_packet_copy();
 *              Don't spend too long in your callback, you're executing on the demux_write() thread path.
 */
typedef void (*demux_callback_pes)(void *userContext, uint16_t pid, struct ltn_pes_packet_s *pes);

/**
 * @brief       Callback function definition, where demuxed and parsed PES frames are delivered
 *              to your function. Caller DOES NOT OWN the lifespan of the returned object.
 *              Don't spend too long in your callback, you're executing on the demux_write() thread path.
 *              TODO description
 */
typedef void (*demux_callback_section)(void *userContext, uint16_t pid, struct ltn_pes_packet_s *pes); /* TODO */

struct ltntstools_demux_callbacks
{
    demux_callback_pes cb_pes;
    demux_callback_section cb_section; /* TODO */
};

/**
 * @brief         Allocate a context for use with other demux api calls.
 *                User must pass in the PAT representing the stream that they plan to demultiplex.
 *                Obtain the PAT from the streammodel framework.
 *                This framework clones the PAT being passed, so you're free to end the PAT lifetime when the call completes.
 *                The user then makes subsequent calls to ltntstools_demux_write() feeding the entire
 *                transport stream into this demux.
 * @param[out]    void **hdl - Unique API context handle
 * @return        0 - Success
 * @return      < 0 - Error
 */
int ltntstools_demux_alloc_from_pat(void **hdl, void *userContext,
    const struct ltntstools_demux_callbacks *callbacks, const struct ltntstools_pat_s *pat);

/**
 * @brief         Free and tear down any resources allocated from this handle.
 * @param[in]     void *hdl - Previously allocate context handle.
 */
void ltntstools_demux_free(void *hdl);

/**
 * @brief       Write an entire MPTS into the framework, any demux happens automatically.
 *              PESs will be parsed and cached, internal metrics updated and where appropriate
 *              any callbacks will fire (on this thread).
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   const uint8_t *pkts - one or more aligned transport packets
 * @param[in]   int packetCount - number of packets
 * @return      number of packets processed, else < 0 on error.
 */
ssize_t ltntstools_demux_write(void *hdl, const uint8_t *pkts, uint32_t packetCount);

#ifdef __cplusplus
};
#endif

#endif /* DEMUX_H */
