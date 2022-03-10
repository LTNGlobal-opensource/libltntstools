#ifndef NAL_H
#define NAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
};
#endif

/**
 * @brief         Search buffer for the byte sequence 000001, a NAL header signature.
 * @param[in]     const uint8_t *buf - Buffer of data, possibly containing none or more NAL packets.
 * @param[in]     int lengthBytes - Buffer length in bytes.
 * @param[in/out] int offset - Enumerator. Caller MUST initalize to -1 before first call.
 *                             Function will use the contents off offset to enumerate the
 *                             entire buffer over multiple calls.
 * @return          0 - Success
 * @return        < 0 - Error
 */
int ltn_nal_findHeader(const uint8_t *buf, int lengthBytes, int *offset);

char *ltn_nal_hevc_findNalTypes(const uint8_t *buf, int lengthBytes);

char *ltn_nal_h264_findNalTypes(const uint8_t *buf, int lengthBytes);

/**
 * @brief         A machanism to find h264 slices in a bitstream, count the number of respective I/P/B frames.
 * @param[in]     uint16_t pid - Specific video pid to analyze. Use 0x2000 to analyze all pids.
 * @return        void * - Success, use this on all future calls into the framework.
 * @return        NULL - Error
 */
void *h264_slice_counter_alloc(uint16_t pid);

/**
 * @brief         A machanism to find h264 slices in a bitstream, count the number of respective I/P/B frames.
 * @param[in]     void *s - Context returned from the prior h264_slice_counter_alloc() call.
 */
void h264_slice_counter_free(void *ctx);

/**
 * @brief         Reset the internal I/P/B frame counts to zero.
 * @param[in]     void *s - Context returned from the prior h264_slice_counter_alloc() call.
 */
void h264_slice_counter_reset(void *ctx);

/**
 * @brief         Reset the internal I/P/B frame counts to zero.
 * @param[in]     void *s - Context returned from the prior h264_slice_counter_alloc() call.
 * @param[in]     int fd - file descriptor that the prinf will occur to.
 * @param[in]     int printZeroCounts - Ensure totals that are zero are printed (1) or discarded(0)
 */
void h264_slice_counter_dprintf(void *ctx, int fd, int printZeroCounts);

/**
 * @brief         Scan the buffer, update the I/P/B counts based on slices found within the buffer. 
 * @param[in]     void *s - Context returned from the prior h264_slice_counter_alloc() call.
 * @param[in]     const unsigned char *pkts - A fully aligned buffer of transport packets.
 * @param[in]     int packetCount - Number of 188 bytes transport packets in the buffer.
 */
void h264_slice_counter_write(void *ctx, const unsigned char *pkts, int packetCount);

#endif /* NAL_H */
