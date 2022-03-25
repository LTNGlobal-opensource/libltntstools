#ifndef _SEGMENTWRITER_H
#define _SEGMENTWRITER_H

/**
 * @file        segmentwriter.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       A threaded file writer. Produces single or segmented recordings.
 *              Capable of supporting any kind of bytestream, targeted at MPEG-TS streams.
 */
#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEGMENTEDWRITER_SINGLE_FILE 0
#define SEGMENTEDWRITER_SEGMENTED   1

/**
 * @brief       Allocate a framework context capable of smoothing MPEG-TS SPTS/MPTS multiplexes.
 * @param[in]   void **hdl - Handle / context for further use.
 * @param[in]   const char *filenamePrefix - Eg '/tmp/myrecording-'
 * @param[in]   const char *filenameSuffix - Eg. '.pcap'
 * @param[in]   int writeMode - SEGMENTEDWRITER_SEGMENTED or SEGMENTEDWRITER_SINGLE_FILE
 * @return      0 on success, else < 0.
 */
int     ltntstools_segmentwriter_alloc(void **hdl, const char *filenamePrefix, const char *filenameSuffix, int writeMode);

/**
 * @brief       Certain types of segments (Ex. PCAP) need a fixed byte structure to be written out
 *              at the beginning of each segment. This function lets you create a 'segment header'
 *              to be prefixed at the beginning of any new segment creation.
 * 
 * @param[in]   void *hdl - Handle / context for further use.
 * @param[in]   const uint8_t *buf - buffer of data
 * @param[in]   size_t lengthBytes - number of bytes
 * @return      0 on success, else < 0.
 */
int     ltntstools_segmentwriter_set_header(void *hdl, const uint8_t *buf, size_t lengthBytes);

/**
 * @brief       Queue data to the writer for later I/O to storage.
 * @param[in]   void *hdl - Handle / context for further use.
 * @param[in]   const uint8_t *buf - buffer of data
 * @param[in]   size_t lengthBytes - number of bytes
 * @return      number of bytes queued, or < 0 on error
 */
ssize_t ltntstools_segmentwriter_write(void *hdl, const uint8_t *buf, size_t lengthBytes);

/**
 * @brief       Free a previously allocate packet, and any attached payload
 * @param[in]   void *hdl - object
 */
void    ltntstools_segmentwriter_free(void *hdl);

/**
 * @brief       Query the current filename being written
 * @param[in]   void *hdl - Handle / context for further use.
 * @param[in]   car *buf - destination
 * @param[in]   int lengthBytes - destination length in bytes
 * @return      0 on success, else < 0.
 */
int     ltntstools_segmentwriter_get_current_filename(void *hdl, char *dst, int lengthBytes);

/**
 * @brief       Return the amount of free space on the filesystem containing the
 *              recordings, as a percentage 1-100.
 * @param[in]   void *hdl - Handle / context for further use.
 * @return      >= 0 on success, else < 0.
 */
double  ltntstools_segmentwriter_get_freespace_pct(void *hdl);

/**
 * @brief       Query the number of segments written.
 * @param[in]   void *hdl - Handle / context for further use.
 * @return      segment count
 */
int     ltntstools_segmentwriter_get_segment_count(void *hdl);

/**
 * @brief       Query the total recording size in bytes, single file or all segments combined.
 * @param[in]   void *hdl - Handle / context for further use.
 * @return      length in bytes
 */
int64_t ltntstools_segmentwriter_get_recording_size(void *hdl);

/**
 * @brief       Query the recording start time.
 * @param[in]   void *hdl - Handle / context for further use.
 * @return      time_t time
 */
time_t  ltntstools_segmentwriter_get_recording_start_time(void *hdl);

/**
 * @brief       In some circumstances, such as PCAP, you may want to
 *              allocate a slot and memory for the queue before you're
 *              ready to queue the final data. This call allocates
 *              and object that you can tamper with, before submitting it
 *              to the queue via ltntstools_segmentwriter_object_write().
 *              You are free to write data to *dst up to lengthBytes
 *              before you queue this for writing.
 * @param[in]   void *hdl - Handle / context for further use.
 * @param[in]   size_t lengthBytes - Handle / context for further use.
 * @param[in]   void **obj - 
 * @param[in]   uint8_t **dst - 
 * @return      time_t time
 */
int     ltntstools_segmentwriter_object_alloc(void *hdl, size_t lengthBytes, void **obj, uint8_t **dst);

/**
 * @brief       Queue object data to the writer for later I/O to storage.
 * @param[in]   void *hdl - Handle / context for further use.
 * @param[in]   void *object - allocated previously via ltntstools_segmentwriter_object_alloc()
 * @return      0 on success, else < 0
 */
int     ltntstools_segmentwriter_object_write(void *hdl, void *object);

/**
 * @brief       Query the latency in the writer, the queue dept of items waiting to
 *              be written to IO. Important to know this when I/O can't sustain the
 *              queue rate, we need to the the I/O writter store to disk before
 *              framework destruction. Monitor the status with this call.
 * @param[in]   void *hdl - Handle / context for further use.
 * @return      int - queue depth.
 */
int     ltntstools_segmentwriter_get_queue_depth(void *hdl);

#ifdef __cplusplus
};
#endif

#endif /* _SEGMENTWRITER_H */


