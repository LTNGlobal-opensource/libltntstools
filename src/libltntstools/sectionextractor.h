#ifndef _SECTIONEXTRACTOR_H
#define _SECTIONEXTRACTOR_H

/**
 * @file        sectionextractor.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       Framework to demux/extract MPEG-TS tables and sections
 *              Heavily leaveraged from bits of libiso13818.
 * 
 */

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       Allocate a framework context capable of table/section demux and extraction
 * @param[in]   void **hdl - Handle / context for further use.
 * @param[in]   uint16_t pid - MPEG TS transport PID to be de-muxed
 * @param[in]   uint8_t tableId - Eg. 0xFC (SCTE35)
 * @return      0 on success, else < 0.
 */
int ltntstools_sectionextractor_alloc(void **hdl, uint16_t pid, uint8_t tableID);

/**
 * @brief       Write an entire MPTS into the framework, pid filtering and demux the stream.
 *              Once an entire PES has been parsed, the caller may query the extracted
 *              table/section once complete is 1, and optionally choose to ignore or make
 *              note of the fact the table was corrupt via crcValid.
 * @param[in]   void *hdl - Handle / context.
 * @param[in]   const uint8_t *pkts - one or more aligned transport packets
 * @param[in]   int packetCount - number of packets
 * @param[out]  int *complete - output flag boolean. zero when parsing, one when caller should call ltntstools_sectionextractor_query()
 * @param[out]  int *crcValid - boolean
 * @return      number of packets processed
 */
ssize_t ltntstools_sectionextractor_write(void *hdl, const uint8_t *pkts, size_t packetCount, int *complete, int *crcValid);

/**
 * @brief       Free a previously allocate context.
 * @param[in]   void *hdl - Handle / context.
 */
void ltntstools_sectionextractor_free(void *hdl);

/**
 * @brief       Allocate a framework context capable of table/section demux and extraction
 * @param[in]   void **hdl - Handle / context for further use.
 * @param[out]  uint8_t *dst - user allocated buffer to contain the raw table/section
 * @param[in]   int lengthBytes - length of the destination buffer.
 * @return      number of bytes written
 */
int ltntstools_sectionextractor_query(void *hdl, uint8_t *dst, int lengthBytes);

#ifdef __cplusplus
};
#endif

#endif /* _SECTIONEXTRACTOR_H */

