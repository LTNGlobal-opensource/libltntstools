/**
 * @file        probes.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2021 LTN Global,Inc. All Rights Reserved.
 * @brief       A module to examine NALS or ISO13818 transport packets for encoder timing information.
 *              Supported encoders today include:
 *                a) LTN Encoder.
 *                   The first use case we'll have is measuring walltime from video frame entering an encoder
 *                   then thorugh the network to this probe instance.
 *
 *               The general design is that each encoder inserts SEI metadata into the stream using customer
 *               SEI unregistered types with a vendor specific UUID. The probe will look for that UUID
 *               and estimate latency based on parsing the vendor specific timing data.
 *
 * Fair warning: The nomenclature will radically evolve around these functions over the coming year.
 *               expect code breakage, function renaming and signature changes as the probes evolve.
 */

#ifndef _PROBES_H
#define _PROBES_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       Allocate a new latency probe, for use with all other calls.
 * @param[out]  void **handle - returned object.
 * @return      0 - Success
 * @return      < 0 - Error
 */
int  ltntstools_probe_ltnencoder_alloc(void **hdl);

/**
 * @brief       Free a previously allocated probe.
 * @param[in]   void *handle - ltntstools_probe_ltnencoder_alloc()
 */
void ltntstools_probe_ltnencoder_free(void *hdl);

/**
 * @brief       Send a buffer of transport packets, or nals, into the query function.
 *              If the function detects the LTN Encoder timing SEI metadatadata, then
 *              timing information is extracted and cached into a context for later query.
 * @param[in]   void *handle - Context returned from ltntstools_probe_ltnencoder_alloc()
 * @return      0 - Success - Caller may query latency via ltntstools_probe_ltnencoder_get_total_latency()
 * @return      < 0 - Error, no timing information detected.
 */
int  ltntstools_probe_ltnencoder_sei_timestamp_query(void *hdl, const unsigned char *buf, int lengthBytes);

/**
 * @brief       Query the detected latency between the frame arrival mechanism of the encoder and
 *              the walltime of this probe. Correct calculations assume bother the encoder and the
 *              platform running this probe are both NTP synced to the same clock.
 * @param[in]   void *handle - Context returned from ltntstools_probe_ltnencoder_alloc()
 * @return      > 0 - Success - Total latency between two measured points expresses in ms.
 * @return      < 0 - Error, no timing information detected.
 */
int64_t ltntstools_probe_ltnencoder_get_total_latency(void *hdl);

#ifdef __cplusplus
};
#endif

#endif /* _PROBES_H */
