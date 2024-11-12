#ifndef PES_H
#define PES_H

/**
 * @file        pes.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       A framework to handle and manipulate ISO13818-1 PES headers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct klbs_context_s;

/**
 * @brief Context used by a packet writer to preserve ongoing state.
 */
struct ltn_pes_packet_writer_ctx
{
	uint64_t nr;
	char dirname[256];
};

/**
 * @brief ISO13818-1 PES packet. See ISO13818 spec table 2.18.
 */
struct ltn_pes_packet_s
{
	uint32_t packet_start_code_prefix;
	uint32_t stream_id;
	uint32_t PES_packet_length;

	uint32_t PES_scrambling_control;
	uint32_t PES_priority;
	uint32_t data_alignment_indicator;
	uint32_t copyright;
	uint32_t original_or_copy;
	uint32_t PTS_DTS_flags;
	uint32_t ESCR_flag;
	uint32_t ES_rate_flag;
	uint32_t DSM_trick_mode_flag;
	uint32_t additional_copy_info_flag;
	uint32_t PES_CRC_flag;
	uint32_t PES_extension_flag;
	uint32_t PES_header_data_length;

	int64_t PTS;
	int64_t DTS;

	uint32_t ES_rate;
	uint32_t additional_copy_info;
	uint32_t previous_PES_packet_CRC;

	uint8_t PES_private_data_flag;
	uint8_t pack_header_field_flag;
	uint8_t program_packet_sequence_counter_flag;
	uint8_t PSTD_buffer_flag;
	uint8_t PSTD_buffer_scale;
	uint32_t PSTD_buffer_size;
	uint8_t PES_extension_flag_2;
	uint8_t PES_extension_field_length;

	unsigned char *data;
	uint32_t dataLengthBytes;

	/* When True, user isn't interested in spending CPU cycles collecting data, optimize it away. */
	uint32_t skipPayloadParsing;

	/* Duplicate buffer of the data used originally to create this PES.
	 * May contain trailing junk that falls outside of this PES, and that's OK.
	 */
	unsigned char *rawBuffer;
	uint32_t rawBufferLengthBytes;
};

/**
 * @brief       Allocate a pes packet, with no payload data.
 * @return      struct ltn_pes_packet_s *pkt - new allocation.
 */
struct ltn_pes_packet_s *ltn_pes_packet_alloc();

/**
 * @brief       Initialize previously allocate packet, and destroy any previously allocated payload data.
 * @param[in]   struct ltn_pes_packet_s *pkt - object
 */
void ltn_pes_packet_init(struct ltn_pes_packet_s *pkt);

/**
 * @brief       Free a previously allocate packet, and any attached payload
 * @param[in]   struct ltn_pes_packet_s *pkt - object
 */
void ltn_pes_packet_free(struct ltn_pes_packet_s *pkt);

/**
 * @brief       Parse an existing bitstream into an existing pkt, returning the number of bits parsed.
 * @param[in]   struct ltn_pes_packet_s *pkt - object
 * @param[in]   struct klbs_context_s *bs - existing bytestream container
 * @param[in]   int skipData - Boolean. Should the parse avoid (for performance reasons) parsing the associated payload data?
 * @return      number of bits parsed, or < 0 on error.
 */
ssize_t ltn_pes_packet_parse(struct ltn_pes_packet_s *pkt, struct klbs_context_s *bs, int skipData);

/**
 * @brief       Helper function. Dump the packet content to console in readable format.
 * @param[in]   struct ltn_pes_packet_s *pkt - object
 * @param[in]   const char *indent - Optional indent string to prefix the printfs with.
 */
void ltn_pes_packet_dump(struct ltn_pes_packet_s *pkt, const char *indent);

/**
 * @brief       Duplicate the PES packet and any attached payload.
 * @param[in]   struct ltn_pes_packet_s *dst - object
 * @param[in]   struct ltn_pes_packet_s *src - object
 */
void ltn_pes_packet_copy(struct ltn_pes_packet_s *dst, struct ltn_pes_packet_s *src);

/**
 * @brief       Helper function. Determine if this PES packet represents audio.
 * @param[in]   struct ltn_pes_packet_s *pes - object
 * @return      Boolean. True or false.
 */
int ltn_pes_packet_is_audio(struct ltn_pes_packet_s *pes);

/**
 * @brief       Helper function. Determine if this PES packet represents video
 * @param[in]   struct ltn_pes_packet_s *pes - object
 * @return      Boolean. True or false.
 */
int ltn_pes_packet_is_video(struct ltn_pes_packet_s *pes);

/**
 * @brief       Pack the pes into a bytestream container
 * @param[in]   struct ltn_pes_packet_s *pes - object
 * @param[in]   struct klbs_context_s *bs - existing bytestream container
 * @return      number of bits packed.
 */
ssize_t ltn_pes_packet_pack(struct ltn_pes_packet_s *pes, struct klbs_context_s *bs);

/**
 * @brief       Initialize a packet writer context.
 * @param[in]   struct ltn_pes_packet_writer_ctx *ctx - object
 * @param[in]   const char *dirname - base directory where file will be recorded.
 * @return      0 on success else < 0.
 */
int ltn_pes_packet_writer_init(struct ltn_pes_packet_writer_ctx *ctx, const char *dirname);

/**
 * @brief       Save the pes ES into a file, in dirname
 * @param[in]   struct ltn_pes_packet_s *pes - object
 * @return      0 on success else < 0.
 */
int ltn_pes_packet_save_es(struct ltn_pes_packet_writer_ctx *ctx, struct ltn_pes_packet_s *pes);

#ifdef __cplusplus
};
#endif

#endif /* PES_H */
