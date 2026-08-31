#ifndef NAL_H264_H
#define NAL_H264_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nals.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief         Search buffer for the byte sequence 000001, a NAL header signature, return an array inside a new
 *                memory allocation for the caller.
 *                CALLER OWNS the array memory allocation, make sure you free it after use.
 * @param[in]     const uint8_t *buf - Buffer of data, possibly containing none or more NAL packets.
 * @param[in]     int lengthBytes - Buffer length in bytes.
 * @param[out]    struct ltn_nal_headers_s **array - Destination pointer for new array allocation
 * @param[out]    int *arrayLength - number of entries in the array.
 * @return          0 - Success
 * @return        < 0 - Error
 */
int ltn_nal_h264_find_headers(const uint8_t *buf, int lengthBytes, struct ltn_nal_headers_s **array, int *arrayLength);

/**
 * @brief         Search buffer for the byte sequence 000001, a NAL header signature.
 * @param[in]     const uint8_t *buf - Buffer of data, possibly containing none or more NAL packets.
 * @param[in]     int lengthBytes - Buffer length in bytes.
 * @param[in,out] int *offset - Enumerator. Caller MUST initalize to -1 before first call.
 *                             Function will use the contents off offset to enumerate the
 *                             entire buffer over multiple calls.
 * @return          0 - Success
 * @return        < 0 - Error
 */
int ltn_nal_h264_findHeader(const uint8_t *buf, int lengthBytes, int *offset);

char *ltn_nal_hevc_findNalTypes(const uint8_t *buf, int lengthBytes);

char *ltn_nal_h264_findNalTypes(const uint8_t *buf, int lengthBytes);

const char *h264Nals_lookupName(int nalType);

/**
 * @brief         A machanism to find h264 slices in a bitstream, count the number of respective I/P/B frames.
 * @param[in]     uint16_t pid - Specific video pid to analyze. Use 0x2000 to analyze all pids.
 * @return        void * - Success, use this on all future calls into the framework.
 * @return        NULL - Error
 */
void *h264_slice_counter_alloc(uint16_t pid);

/**
 * @brief         Query the pid assocuated with the current counter;
 * @param[in]     void *ctx - Context returned from the prior h264_slice_counter_alloc() call.
 * @return        0 thru 0x2000
 */
uint16_t h264_slice_counter_get_pid(void *ctx);

/**
 * @brief         A machanism to find h264 slices in a bitstream, count the number of respective I/P/B frames.
 * @param[in]     void *ctx - Context returned from the prior h264_slice_counter_alloc() call.
 */
void h264_slice_counter_free(void *ctx);

/**
 * @brief         Reset the internal I/P/B frame counts to zero.
 * @param[in]     void *ctx - Context returned from the prior h264_slice_counter_alloc() call.
 */
void h264_slice_counter_reset(void *ctx);

/**
 * @brief         Reset the internal I/P/B frame counts to zero, adn establish a pid to slice count;
 * @param[in]     void *ctx - Context returned from the prior h264_slice_counter_alloc() call.
 * @param[in]     uint16_t pid - Specific video pid to analyze. Use 0x2000 to analyze all pids.
 */
void h264_slice_counter_reset_pid(void *ctx, uint16_t pid);

/**
 * @brief         Reset the internal I/P/B frame counts to zero.
 * @param[in]     void *ctx - Context returned from the prior h264_slice_counter_alloc() call.
 * @param[in]     int fd - file descriptor that the prinf will occur to.
 * @param[in]     int printZeroCounts - Ensure totals that are zero are printed (1) or discarded(0)
 */
void h264_slice_counter_dprintf(void *ctx, int fd, int printZeroCounts);

/**
 * @brief         Scan the buffer, update the I/P/B counts based on slices found within the buffer.
 * @param[in]     void *ctx - Context returned from the prior h264_slice_counter_alloc() call.
 * @param[in]     const unsigned char *pkts - A fully aligned buffer of transport packets.
 * @param[in]     int packetCount - Number of 188 bytes transport packets in the buffer.
 */
void h264_slice_counter_write(void *ctx, const unsigned char *pkts, int packetCount);

struct h264_slice_counter_results_s
{
    uint64_t i;
    uint64_t b;
    uint64_t p;
    uint64_t si;
    uint64_t sp;

#define H264_SLICE_COUNTER_HISTORY_LENGTH 20
    char sliceHistory[H264_SLICE_COUNTER_HISTORY_LENGTH + 1];
};
void h264_slice_counter_query(void *ctx, struct h264_slice_counter_results_s *results);

const char *h264_slice_name_ascii(int slice_type);

int h264_nal_get_slice_type(const struct ltn_nal_headers_s *hdr, char *sliceType);

int h264_is_slice_type_iframe(unsigned int sliceType);
int h264_is_slice_type_bframe(unsigned int sliceType);
int h264_is_slice_type_pframe(unsigned int sliceType);

/**
 * @brief         Return the AVC slice_type ror a given nal structure.
 * @param[in]     struct ltn_nal_headers_s *hdr - nal header.
 * @param[out]    unsigned int *sliceType - A valid slice_type as identified in the AVC specification.
 * @return        0 on success else < 0
 */
int h264_nal_get_slice_type_for_nal(struct ltn_nal_headers_s *hdr, unsigned int *sliceType);

int ltn_sei_h264_find_headers(struct ltn_nal_headers_s *nals, int nalArrayLength, struct ltn_sei_headers_s **array, int *arrayLength);

/* Guaranteed to return a usable string, even if the sei Type is invalid */
const char *ltn_sei_h264_lookupName(int seiType);

/**
 * @brief         Strip the RBSP emulation prevents bytes from a nal header. Necessary for SEI processing typically.
 *                The ptr field buffer and lengthBytes fields are adjusted.
 * @param[in,out] struct ltn_nal_headers_s *h - Nal to modify
 * @return          0 - Success
 * @return        < 0 - Error
 */
int ltn_nal_h264_strip_emulation_prevention(struct ltn_nal_headers_s *h);

/**
 * @brief         A single ClockTS() entry decoded from an H.264 SEI PIC_TIMING message,
 *                per ISO-14496-10 Annex D.1/D.2.3.
 */
struct ltn_nal_h264_pic_timing_clock_s
{
	int present;                 /* clock_timestamp_flag. When 0, no other field in this struct is valid. */
	int ct_type;
	int nuit_field_based_flag;
	int counting_type;
	int full_timestamp_flag;
	int discontinuity_flag;
	int cnt_dropped_flag;
	int n_frames;
	int seconds;                 /* -1 when not present in the bitstream */
	int minutes;                 /* -1 when not present in the bitstream */
	int hours;                   /* -1 when not present in the bitstream */
};

/* Per ISO-14496-10 Table D-1, pic_struct is a 4-bit field (0-15) and NumClockTS never exceeds 3. */
#define LTN_NAL_H264_PIC_TIMING_MAX_CLOCKS 3

struct ltn_nal_h264_pic_timing_s
{
	int pic_struct;
	int clockCount;              /* Number of valid entries in .clocks[] (NumClockTS) */
	struct ltn_nal_h264_pic_timing_clock_s clocks[LTN_NAL_H264_PIC_TIMING_MAX_CLOCKS];
};

/**
 * @brief         Parse an H.264 SEI PIC_TIMING message (nal_unit_type 6, payloadType 1) into
 *                structured fields. Caller must have already stripped RBSP emulation prevention
 *                bytes (see ltn_nal_h264_strip_emulation_prevention()).
 *                This parser assumes CpbDpbDelaysPresentFlag and pic_struct_present_flag are both
 *                true and time_offset_length is 0, matching the VUI/HRD configuration this
 *                implementation supports.
 * @param[in]     const uint8_t *buf - NAL buffer, starting with the 00 00 01 06 start code prefix
 *                (matches struct ltn_nal_headers_s .ptr).
 * @param[in]     int lengthBytes - Length of buf in bytes (matches struct ltn_nal_headers_s .lengthBytes).
 * @param[in]     int cpb_removal_delay_length - cpb_removal_delay_length in bits, from the active
 *                SPS VUI HRD parameters. This parser has no access to the SPS, so the caller must
 *                supply it.
 * @param[in]     int dpb_removal_delay_length - dpb_removal_delay_length in bits, from the active
 *                SPS VUI HRD parameters.
 * @param[in]     int pic_struct_override - When >= 0, force this pic_struct value instead of the
 *                one read from the bitstream (useful for encoders known to populate pic_struct
 *                incorrectly). Pass -1 to always use the value read from the bitstream.
 * @param[out]    struct ltn_nal_h264_pic_timing_s *result - Parsed results.
 * @return          0 - Success
 * @return        < 0 - Error (NULL args, buffer too short, or invalid pic_struct)
 */
int ltn_nal_h264_parse_pic_timing(const uint8_t *buf, int lengthBytes,
	int cpb_removal_delay_length, int dpb_removal_delay_length, int pic_struct_override,
	struct ltn_nal_h264_pic_timing_s *result);

#ifdef __cplusplus
};
#endif

#endif /* NAL_H264_H */
