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

struct h264_slice_data_s
{
	uint32_t  slice_type;
	uint64_t  count;
	char     *name;
};

struct h264_slice_data_s *h264_slice_counter_alloc();
void h264_slice_counter_free(struct h264_slice_data_s *s);
void h264_slice_counter_reset(struct h264_slice_data_s *s);
void h264_slice_counter_update(struct h264_slice_data_s *s, int slice_type);
void h264_slice_counter_dprintf(struct h264_slice_data_s *s, int fd, int printZeroCounts);
void h264_slice_counter_write(struct h264_slice_data_s *s, const unsigned char *pkts, int packetCount);

#endif /* NAL_H */
