#ifndef LIBLTNTSTOOLS_VBV_H
#define LIBLTNTSTOOLS_VBV_H

/**
 * @file        vbv.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2025 LTN Global,Inc. All Rights Reserved.
 * @brief       ISO13818-1 Video Buffer Verifier (VBV) implementation.
 *              Validate that a given PID in a TS with its PCR, isn't
 *              violating a buffer model.
 * 
 * Framework is designed to accept units of struct ltn_pes_packet_s.
 * Encoders (via a transport system) have packetized TS.
 * TS is unwrapped and converted into PES (ES) and pushed into
 * the VBV ltntstools_vbv_write() in various video team technologies.
 * A virtual minimal decoder pulls frames from the VBV on a framerate
 * schedule.
 * 
 * The fields in ltn_pes_packet_s that are used, are:
 *  PTS_DTS_flags
 *  PTS
 *  DTS
 *  rawBufferLengthBytes
 * 
 * (Encoders or decoders).
 * The goal of this framework is to help understand if the PES is
 * too bursty on the network, likely to violate a Video Buffer Verifier
 * guarantee.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define VBV_CODEC_H264  1
#define VBV_CODEC_H265  2 /**< Not yet supported */
#define VBV_CODEC_MPEG2 3 /**< Not yet supported */

enum ltntstools_vbv_event_e {
	EVENT_VBV_UNDEFINED = 0,
	EVENT_VBV_FULLNESS_PCT,   /**< triggered when fullnes < 10% or > 90% */
	EVENT_VBV_BPS,
	EVENT_VBV_UNDERFLOW,      /**< Buffering violation */
	EVENT_VBV_OVERFLOW,       /**< Buffering violation */
	EVENT_VBV_OOO_DTS,        /**< DTS violation, DTS's are in violation */
};

/**
 * @brief       Callback function definition.
 */
typedef void (*vbv_callback)(void *userContext, enum ltntstools_vbv_event_e event);

struct vbv_decoder_profile_s
{
    uint32_t vbv_buffer_size; /**< Optional (Bytes). 0 will assume the default 819200 buffer. See ISO14496-10 Annex A */
    double framerate;         /**< Mandatory: 23.98, 24, 25, 29.97, 30, 50, 59.94, or 60. */
};

/**
 * @brief       For a given codec, Eg. VBV_CODEC_H264, IDC and framrate, configure the VBV buffer profile.
 *              Do this before calling ltntstools_vbv_alloc(). Don't forget to validate the profile
 *              using ltntstools_vbv_profile_validate().
 * @param[in]   struct vbv_decoder_profile_s - VBV decoder profile
 * @return      True on value, else false,
 */
int ltntstools_vbv_profile_validate(struct vbv_decoder_profile_s *dp);

/**
 * @brief       Allocate a framework context capable of demuxing and parsing PES streams.
 * @param[in]   void **hdl - Handle / context for further use.
 * @param[in]   uint16_t pid - MPEG TS transport PID to be de-muxed
 * @param[in]   vbv_callback cb - user supplied callback for PES frame delivery
 * @param[in]   void *userContext - user private context, passed back to caller during callback.
 * @param[in]   struct vbv_decoder_profile_s * - Expected decoder profile
 * @return      0 on success, else < 0.
 */
int ltntstools_vbv_alloc(void **hdl, uint16_t pid, vbv_callback cb, void *userContext, struct vbv_decoder_profile_s *p);

/**
 * @brief       Free a previously allocated context.
 * @param[in]   void *hdl - Handle / context.
 */
void ltntstools_vbv_free(void *hdl);

/**
 * @brief       Send a PES into the VBV. update any internal statistics, fire any events if needed.
 * @param[in]   void *hdl - Handle / context for further use.
 * @param[in]   const struct ltn_pes_packet_s * - Timing and payload information
 * @return      0 on success, else < 0.
 */
int ltntstools_vbv_write(void *hdl, const struct ltn_pes_packet_s *pkt);

/**
 * @brief       Convert an event name into a human readable string.
 * @param[in]   enum ltntstools_vbv_event_e - eventId
 * @return      The event name. A string is guaranteed to be returned from the stack, in all cases.
 */
const char *ltntstools_vbv_event_name(enum ltntstools_vbv_event_e e);

/**
 * @brief       For a given codec, Eg. VBV_CODEC_H264, lookup the VBV buffersize for a given IDC (level)
 * @param[in]   int codec - VBV_CODEC_H264
 * @param[in]   int levelX10 - The level, or ODC, Eg. for 3.1 use 31
 * @return      0 on success, else < 0.
 */
int ltntstools_vbv_bitrate_lookup(int codec, int levelX10);

/**
 * @brief       For a given codec, Eg. VBV_CODEC_H264, IDC and framrate, configure the VBV buffer profile.
 *              Do this before calling ltntstools_vbv_alloc(). Don't forget to validate the profile
 *              using ltntstools_vbv_profile_validate().
 * @param[in]   int codec - VBV_CODEC_H264
 * @param[in]   int levelX10 - The level, or ODC, Eg. for 3.1 use 31
 * @return      0 on success, else < 0.
 */
int ltntstools_vbv_profile_defaults(struct vbv_decoder_profile_s *p, int codec, int levelX10, double framerate);

#ifdef __cplusplus
};
#endif

#endif /* LIBLTNTSTOOLS_VBV_H */
