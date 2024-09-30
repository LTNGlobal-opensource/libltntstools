#include "libltntstools/pes.h"
#include "klbitstream_readwriter.h"
#include <inttypes.h>

#include <libltntstools/ltntstools.h>

#define DISPLAY_U32(indent, fn) printf("%s%s = %d (0x%x)\n", indent, #fn, fn, fn);
#define DISPLAY_U32_NOCR(indent, fn) printf("%s%s = %d (0x%x)", indent, #fn, fn, fn);
#define DISPLAY_U64(indent, fn) printf("%s%s = %" PRIu64 " (0x%" PRIx64 ")\n", indent, #fn, fn, fn);
#define DISPLAY_U32_SUFFIX(indent, fn, str) printf("%s%s = %d (0x%x) %s\n", indent, #fn, fn, fn, str);

struct ltn_pes_packet_s *ltn_pes_packet_alloc()
{
	struct ltn_pes_packet_s *pkt = calloc(1, sizeof(*pkt));
	return pkt;
}

void ltn_pes_packet_init(struct ltn_pes_packet_s *pkt)
{
	if (pkt->rawBuffer) {
		free(pkt->rawBuffer);
	}
	if (pkt->data) {
		free(pkt->data);
	}
	memset(pkt, 0, sizeof(*pkt));
}

void ltn_pes_packet_free(struct ltn_pes_packet_s *pkt)
{
	if (pkt->rawBuffer) {
		free(pkt->rawBuffer);
		pkt->rawBuffer = NULL;
		pkt->rawBufferLengthBytes = 0;
	}
	if (pkt->data) {
		free(pkt->data);
		pkt->data = NULL;
		pkt->dataLengthBytes = 0;
	}
	free(pkt);
}

static void write33bit_ts(struct klbs_context_s *bs, int64_t value)
{
	klbs_write_bits(bs, value >> 30, 3);
	klbs_write_bits(bs, 1, 1);

	klbs_write_bits(bs, value >> 15, 15);
	klbs_write_bits(bs, 1, 1);

	klbs_write_bits(bs, value, 15);
	klbs_write_bits(bs, 1, 1);
}

static int64_t read33bit_ts(struct klbs_context_s *bs)
{
        int64_t a = (uint64_t)klbs_read_bits(bs, 3) << 30;
        if (klbs_read_bits(bs, 1) != 1)
                return -1;

        int64_t b = (uint64_t)klbs_read_bits(bs, 15) << 15;
        if (klbs_read_bits(bs, 1) != 1)
                return -1;

        int64_t c = (uint64_t)klbs_read_bits(bs, 15);
        if (klbs_read_bits(bs, 1) != 1)
                return -1;

	int64_t ts = a | b | c;

	return ts;
}

ssize_t ltn_pes_packet_pack(struct ltn_pes_packet_s *pkt, struct klbs_context_s *bs)
{
	ssize_t bits = 0;
	klbs_write_bits(bs, pkt->packet_start_code_prefix, 24);
	klbs_write_bits(bs, pkt->stream_id, 8);
	klbs_write_bits(bs, pkt->PES_packet_length, 16);

	if ((pkt->stream_id != 0xBC /* program_stream_map */) &&
		(pkt->stream_id != 0xBE /* padding_stream */) &&
		(pkt->stream_id != 0xBF /* private_stream_2 */) &&
		(pkt->stream_id != 0xF0 /* ECM */) &&
		(pkt->stream_id != 0xF1 /* EMM */) &&
		(pkt->stream_id != 0xFF /* program_stream_directory */) &&
		(pkt->stream_id != 0xF2 /* DSMCC_stream */) &&
		(pkt->stream_id != 0xF8 /* ITU H.222.1 type E */))
	{

		klbs_write_bits(bs, 0xff, 2); /* reserved */

		klbs_write_bits(bs, pkt->PES_scrambling_control, 2);
		klbs_write_bits(bs, pkt->PES_priority, 1);
		klbs_write_bits(bs, pkt->data_alignment_indicator, 1);
		klbs_write_bits(bs, pkt->copyright, 1);
		klbs_write_bits(bs, pkt->original_or_copy, 1);
		klbs_write_bits(bs, pkt->PTS_DTS_flags, 2);
		klbs_write_bits(bs, pkt->ESCR_flag, 1);
		klbs_write_bits(bs, pkt->ES_rate_flag, 1);
		klbs_write_bits(bs, pkt->DSM_trick_mode_flag, 1);
		klbs_write_bits(bs, pkt->additional_copy_info_flag, 1);
		klbs_write_bits(bs, pkt->PES_CRC_flag, 1);
		klbs_write_bits(bs, pkt->PES_extension_flag, 1);
		klbs_write_bits(bs, pkt->PES_header_data_length, 8);

		bits += 72;

		if (pkt->PTS_DTS_flags == 2) {
			klbs_write_bits(bs, 0x2, 4);
			write33bit_ts(bs, pkt->PTS);
			bits += 40;
		} else
		if (pkt->PTS_DTS_flags == 3) {
			klbs_write_bits(bs, 0x3, 4);
			write33bit_ts(bs, pkt->PTS);
			bits += 40;

			klbs_write_bits(bs, 0x1, 4);
			write33bit_ts(bs, pkt->DTS);
			bits += 40;
		}

		if (pkt->ESCR_flag) {
			klbs_write_bits(bs, 0, 40); /* Not supported */
			bits += 48;
		}

		if (pkt->ES_rate_flag) {
			klbs_write_bits(bs, 1, 1); /* market bit */
			klbs_write_bits(bs, pkt->ES_rate_flag, 22);
			klbs_write_bits(bs, 1, 1); /* market bit */
			bits += 24;
		}

		if (pkt->DSM_trick_mode_flag) {
			klbs_write_bits(bs, 0, 8); /* Not supported */ /* Skip trick mode bits */
			bits += 8;
		}

		if (pkt->additional_copy_info_flag) {
			klbs_write_bits(bs, 1, 1); /* market bit */
			klbs_write_bits(bs, pkt->additional_copy_info, 7);
			bits += 8;
		}

		if (pkt->PES_CRC_flag) {
			klbs_write_bits(bs, 0, 16); /* Not supported */
			bits += 16;
		}

		if (pkt->PES_extension_flag) {
			klbs_write_bits(bs, pkt->PES_private_data_flag, 1);
			klbs_write_bits(bs, pkt->pack_header_field_flag, 1);
			klbs_write_bits(bs, pkt->program_packet_sequence_counter_flag, 1);
			klbs_write_bits(bs, pkt->PSTD_buffer_flag, 1);
			klbs_write_bits(bs, 0xff, 3); /* reserved */
			klbs_write_bits(bs, pkt->PES_extension_flag_2, 1);
			bits += 8;

			if (pkt->PES_private_data_flag == 1) {
				klbs_write_bits(bs, 0xffffffff, 32); /* private data */
				klbs_write_bits(bs, 0xffffffff, 32); /* private data */
				klbs_write_bits(bs, 0xffffffff, 32); /* private data */
				klbs_write_bits(bs, 0xffffffff, 32); /* private data */
				bits += 128;
			}

			if (pkt->pack_header_field_flag == 1) {
				/* Not supported */
//					bits += 8;
			}

			if (pkt->program_packet_sequence_counter_flag == 1) {
				/* Not supported */
//				klbs_read_bits(bs, 16);
				bits += 16;
			}

			if (pkt->PSTD_buffer_flag == 1) {
				/* Not supported */
//				klbs_read_bits(bs, 2); /* '01' */
//				pkt->PSTD_buffer_scale = klbs_read_bits(bs, 1);
//				pkt->PSTD_buffer_size = klbs_read_bits(bs, 13);
				bits += 16;
			}

			if (pkt->PES_extension_flag_2 == 1) {
				bits += 8;
				for (int i = 0; i < pkt->PES_extension_field_length; i++) {
					klbs_write_bits(bs, 0xff, 8); /* Not supported */
					bits += 8;
				}
			}
		}

		for (int i = 0; i < pkt->dataLengthBytes; i++) {
			klbs_write_bits(bs, *(pkt->data + i), 8);
			bits += 8;
		}
	} /* (pkt->stream_id != 0xBC) && */
	else if ((pkt->stream_id == 0xBF /* private_stream_2 */) ||
		(pkt->stream_id == 0xF0 /* ECM */) ||
		(pkt->stream_id == 0xF1 /* EMM */) ||
		(pkt->stream_id == 0xFF /* program_stream_directory */) ||
		(pkt->stream_id == 0xF2 /* DSMCC_stream */) ||
		(pkt->stream_id == 0xF8 /* H.222.1 type E */))
	{
		if (pkt->data) {
			for (int i = 0; i < pkt->PES_packet_length; i++) {
				klbs_write_bits(bs, *(pkt->data + i), 8);
				bits += 8;
			}
		} else
		if (pkt->stream_id == 0xBE /* padding_stream */) {
			for (int i = 0; i < pkt->PES_packet_length; i++) {
				klbs_write_bits(bs, 0xff, 8); /* padding */
				bits += 8;
			}
		}
	}

	return bits;
}

ssize_t ltn_pes_packet_parse(struct ltn_pes_packet_s *pkt, struct klbs_context_s *bs, int skipData)
{
	ssize_t bits = 0;

	/* Make sure something exists in the buffer */
	if (klbs_get_byte_count_free(bs) < 8)
		return bits;

	/* Clone the entire buffer into a raw duplicate. */

	pkt->skipPayloadParsing = skipData;

	/* TODO: This still needs a little work, esp around trick play.
	 * Read the spec, see what's missing and add it.
	 */
	pkt->packet_start_code_prefix = klbs_read_bits(bs, 24);
	pkt->stream_id = klbs_read_bits(bs, 8);
	pkt->PES_packet_length = klbs_read_bits(bs, 16);

	if ((pkt->stream_id != 0xBC /* program_stream_map */) &&
		(pkt->stream_id != 0xBE /* padding_stream */) &&
		(pkt->stream_id != 0xBF /* private_stream_2 */) &&
		(pkt->stream_id != 0xF0 /* ECM */) &&
		(pkt->stream_id != 0xF1 /* EMM */) &&
		(pkt->stream_id != 0xFF /* program_stream_directory */) &&
		(pkt->stream_id != 0xF2 /* DSMCC_stream */) &&
		(pkt->stream_id != 0xF8 /* ITU H.222.1 type E */))
	{

		klbs_read_bits(bs, 2); /* reserved */

		pkt->PES_scrambling_control = klbs_read_bits(bs, 2);
		pkt->PES_priority = klbs_read_bits(bs, 1);
		pkt->data_alignment_indicator = klbs_read_bits(bs, 1);
		pkt->copyright = klbs_read_bits(bs, 1);
		pkt->original_or_copy = klbs_read_bits(bs, 1);
		pkt->PTS_DTS_flags = klbs_read_bits(bs, 2);
		pkt->ESCR_flag = klbs_read_bits(bs, 1);
		pkt->ES_rate_flag = klbs_read_bits(bs, 1);
		pkt->DSM_trick_mode_flag = klbs_read_bits(bs, 1);
		pkt->additional_copy_info_flag = klbs_read_bits(bs, 1);
		pkt->PES_CRC_flag = klbs_read_bits(bs, 1);
		pkt->PES_extension_flag = klbs_read_bits(bs, 1);
		pkt->PES_header_data_length = klbs_read_bits(bs, 8);

		bits += 72;

		if (pkt->PTS_DTS_flags == 2) {
			klbs_read_bits(bs, 4); /* 0010 */
			pkt->PTS = read33bit_ts(bs);
			bits += 40;
		} else
		if (pkt->PTS_DTS_flags == 3) {
			klbs_read_bits(bs, 4); /* 0011 */
			pkt->PTS = read33bit_ts(bs);
			bits += 40;

			klbs_read_bits(bs, 4); /* 0001 */
			pkt->DTS = read33bit_ts(bs);
			bits += 40;
		}

		if (pkt->ESCR_flag) {
			klbs_read_bits(bs, 48); /* Skip the ESCR base */
			bits += 48;
		}

		if (pkt->ES_rate_flag) {
			klbs_read_bits(bs, 1); /* marker bit */
			pkt->ES_rate_flag = klbs_read_bits(bs, 22);
			klbs_read_bits(bs, 1); /* marker bit */
			bits += 24;
		}

		if (pkt->DSM_trick_mode_flag) {
			klbs_read_bits(bs, 8); /* Skip trick mode bits */
			bits += 8;
		}

		if (pkt->additional_copy_info_flag) {
			klbs_read_bits(bs, 1); /* marker bit */
			pkt->additional_copy_info = klbs_read_bits(bs, 7);
			bits += 8;
		}

		if (pkt->PES_CRC_flag) {
			pkt->previous_PES_packet_CRC = klbs_read_bits(bs, 16);
			bits += 16;
		}

		if (pkt->PES_extension_flag) {
			pkt->PES_private_data_flag = klbs_read_bits(bs, 1);
			pkt->pack_header_field_flag = klbs_read_bits(bs, 1);
			pkt->program_packet_sequence_counter_flag = klbs_read_bits(bs, 1);
			pkt->PSTD_buffer_flag = klbs_read_bits(bs, 1);
			klbs_read_bits(bs, 3); /* reserved */
			pkt->PES_extension_flag_2 = klbs_read_bits(bs, 1);
			bits += 8;

			if (pkt->PES_private_data_flag == 1) {
				klbs_read_bits(bs, 32); /* private data */
				klbs_read_bits(bs, 32); /* private data */
				klbs_read_bits(bs, 32); /* private data */
				klbs_read_bits(bs, 32); /* private data */
				bits += 128;
			}

			if (pkt->pack_header_field_flag == 1) {
				int len = klbs_read_bits(bs, 8);
				bits += 8;
				for (int i = 0; i < len; i++) {
					klbs_read_bits(bs, 8); /* No support */
					bits += 8;
				}
			}

			if (pkt->program_packet_sequence_counter_flag == 1) {
				klbs_read_bits(bs, 16);
				bits += 16;
			}

			if (pkt->PSTD_buffer_flag == 1) {
				klbs_read_bits(bs, 2); /* '01' */
				pkt->PSTD_buffer_scale = klbs_read_bits(bs, 1);
				pkt->PSTD_buffer_size = klbs_read_bits(bs, 13);
				bits += 16;
			}

			if (pkt->PES_extension_flag_2 == 1) {
				klbs_read_bits(bs, 1); /* market bit */
				pkt->PES_extension_field_length = klbs_read_bits(bs, 7);
				bits += 8;
				/* check if we overrun the buffer here */
				int byte_count_free = klbs_get_byte_count_free(bs);
				if (byte_count_free < 0) {
					fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Byte count free is negative %d\n",
							__FILE__, __func__, __LINE__, pkt->stream_id, byte_count_free);
					byte_count_free = 0;
				}
				if (byte_count_free < pkt->PES_extension_field_length || pkt->PES_extension_field_length < 0) {
#if KLBITSTREAM_DEBUG
					fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Packet Parse PES_extension_field_length %d, but only %d bytes left in buffer\n",
							__FILE__, __func__, __LINE__, pkt->stream_id, pkt->PES_extension_field_length, byte_count_free);
#endif
#if KLBITSTREAM_TRUNCATE_ON_OVERRUN
					pkt->PES_extension_field_length = byte_count_free;
					/* measure what the actual PES_packet_length should be */					
					pkt->PES_packet_length = (bits - 48) / 8 + pkt->PES_extension_field_length;
					if (pkt->PES_packet_length < 0) {
						pkt->PES_packet_length = 0;
					}
					bs->truncated = 1;
#elif KLBITSTREAM_RETURN_ON_OVERRUN
					bs->overrun = 1;
					return bits;
#endif
				}
				for (int i = 0; i < pkt->PES_extension_field_length; i++) {
					klbs_read_bits(bs, 8); /* reserved */
					bits += 8;
				}
			}
		}

		pkt->dataLengthBytes = 0;
		pkt->data = NULL;

		if (skipData) {
		} else {
			/* check if our buffer is big enough for the rest of the packet */
			int byte_count_free = klbs_get_byte_count_free(bs);
			if (byte_count_free < 0) {
				fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Byte count free is negative %d\n",
						__FILE__, __func__, __LINE__, pkt->stream_id, byte_count_free);
				byte_count_free = 0;
			}
			if (pkt->PES_packet_length >= ((bits - 48) / 8)) { /* PES_packet_length is valid */
				pkt->dataLengthBytes = pkt->PES_packet_length - ((bits - 48) / 8);
			} else {
				pkt->dataLengthBytes = byte_count_free;
			}
			if (pkt->dataLengthBytes > byte_count_free) {
#if KLBITSTREAM_DEBUG
				fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Packet Parse PES_packet_length %d dataLengthBytes %d, but only %d bytes left in buffer\n",
						__FILE__, __func__, __LINE__, pkt->stream_id, pkt->PES_packet_length, pkt->dataLengthBytes, byte_count_free);
#endif
#if KLBITSTREAM_TRUNCATE_ON_OVERRUN
				pkt->dataLengthBytes = byte_count_free;
				bs->truncated = 1;
#elif KLBITSTREAM_RETURN_ON_OVERRUN
				bs->overrun = 1;
				return bits;
#endif
			}

			/* Handle data */
			pkt->data = malloc(pkt->dataLengthBytes);
			if (pkt->data) {
				for (int i = 0; i < pkt->dataLengthBytes; i++) {
					*(pkt->data + i) = klbs_read_bits(bs, 8);
					bits += 8;
				}
			} else {
				pkt->dataLengthBytes = 0;
			}
		}
	} /* (pkt->stream_id != 0xBC) && */
	else if ((pkt->stream_id == 0xBF /* private_stream_2 */) ||
		(pkt->stream_id == 0xF0 /* ECM */) ||
		(pkt->stream_id == 0xF1 /* EMM */) ||
		(pkt->stream_id == 0xFF /* program_stream_directory */) ||
		(pkt->stream_id == 0xF2 /* DSMCC_stream */) ||
		(pkt->stream_id == 0xF8 /* H.222.1 type E */))
	{
		/* check if our buffer is big enough for the rest of the packet */
		int byte_count_free = klbs_get_byte_count_free(bs);
		if (byte_count_free < 0) {
			fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Byte count free is negative %d\n",
					__FILE__, __func__, __LINE__, pkt->stream_id, byte_count_free);
			byte_count_free = 0;
		}

		if (byte_count_free < pkt->PES_packet_length || pkt->PES_packet_length < 0) {
#if KLBITSTREAM_DEBUG
			fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Packet Parse PES_packet_length %d, but only %d bytes left in buffer\n",
					__FILE__, __func__, __LINE__, pkt->stream_id, pkt->PES_packet_length, byte_count_free);
#endif
#if KLBITSTREAM_TRUNCATE_ON_OVERRUN
			pkt->PES_packet_length = byte_count_free;
			bs->truncated = 1;
#elif KLBITSTREAM_RETURN_ON_OVERRUN
			bs->overrun = 1;
			return bits;
#endif
		}
		pkt->data = malloc(pkt->PES_packet_length);
		if (pkt->data) {
			for (int i = 0; i < pkt->PES_packet_length; i++) {
				*(pkt->data + i) = klbs_read_bits(bs, 8); /* PES_packet_data_byte */
				bits += 8;
			}
		} else {
			pkt->dataLengthBytes = 0;
		}
	} else if (pkt->stream_id == 0xBE /* padding_stream */) {
		/* check if our buffer is big enough for the rest of the packet */
		int byte_count_free = klbs_get_byte_count_free(bs);
		if (byte_count_free < 0) {
			fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Byte count free is negative %d\n",
					__FILE__, __func__, __LINE__, pkt->stream_id, byte_count_free);
			byte_count_free = 0;
		}

		if (byte_count_free < pkt->PES_packet_length || pkt->PES_packet_length < 0) {
#if KLBITSTREAM_DEBUG
			fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) PES id 0x%04x Packet Parse PES_packet_length %d, but only %d bytes left in buffer\n",
					__FILE__, __func__, __LINE__, pkt->stream_id, pkt->PES_packet_length, byte_count_free);
#endif
#if KLBITSTREAM_TRUNCATE_ON_OVERRUN
			pkt->PES_packet_length = byte_count_free;
			bs->truncated = 1;
#elif KLBITSTREAM_RETURN_ON_OVERRUN
			bs->overrun = 1;
			return bits;
#endif
		}
		for (int i = 0; i < pkt->PES_packet_length; i++) {
			klbs_read_bits(bs, 8); /* padding_byte */
			bits += 8;
		}
	}

	return bits;
}

void ltn_pes_packet_dump(struct ltn_pes_packet_s *pkt, const char *indent)
{
	char i[32];
	sprintf(i, "%s    ", indent);

	DISPLAY_U32(indent, pkt->packet_start_code_prefix);
	DISPLAY_U32_SUFFIX(i, pkt->stream_id,
		ltn_pes_packet_is_video(pkt) ? "[VIDEO]" :
		ltn_pes_packet_is_audio(pkt) ? "[AUDIO]" : "[OTHER]");

	DISPLAY_U32(i, pkt->PES_packet_length);
	DISPLAY_U32(i, pkt->PES_scrambling_control);
	DISPLAY_U32(i, pkt->PES_priority);
	DISPLAY_U32(i, pkt->data_alignment_indicator);
	DISPLAY_U32(i, pkt->copyright);
	DISPLAY_U32(i, pkt->original_or_copy);
	DISPLAY_U32(i, pkt->PTS_DTS_flags);
	DISPLAY_U32(i, pkt->ESCR_flag);
	DISPLAY_U32(i, pkt->ES_rate_flag);
	DISPLAY_U32(i, pkt->DSM_trick_mode_flag);
	DISPLAY_U32(i, pkt->additional_copy_info_flag);
	DISPLAY_U32(i, pkt->PES_CRC_flag);
	DISPLAY_U32(i, pkt->PES_extension_flag);
	DISPLAY_U32(i, pkt->PES_header_data_length);
	if (pkt->PTS_DTS_flags == 2) {
		DISPLAY_U64(i, pkt->PTS);
	} else
	if (pkt->PTS_DTS_flags == 3) {
		DISPLAY_U64(i, pkt->PTS);
		DISPLAY_U64(i, pkt->DTS);
	}

	if (pkt->ES_rate_flag) {
		DISPLAY_U32(i, pkt->ES_rate);
	}
	if (pkt->additional_copy_info) {
		DISPLAY_U32(i, pkt->additional_copy_info);
	}

	if (pkt->PES_CRC_flag) {
		DISPLAY_U32(i, pkt->previous_PES_packet_CRC);
	}

	if (pkt->skipPayloadParsing) {
		DISPLAY_U32_NOCR(i, pkt->dataLengthBytes);
		printf(" (operator opted out of parsing ~%d payload bytes)\n", pkt->PES_packet_length - pkt->PES_header_data_length);
	} else {
		DISPLAY_U32(i, pkt->dataLengthBytes);
	}
	if (pkt->dataLengthBytes) {
		ltntstools_hexdump(pkt->data, pkt->dataLengthBytes, 16);
	}
}

void ltn_pes_packet_copy(struct ltn_pes_packet_s *dst, struct ltn_pes_packet_s *src)
{
	memcpy(dst, src, sizeof(*src));
	if (src->data) {
		dst->data = malloc(src->dataLengthBytes);
		memcpy(dst->data, src->data, src->dataLengthBytes);
	}
	if (src->rawBuffer) {
		dst->rawBuffer = malloc(src->rawBufferLengthBytes);
		memcpy(dst->rawBuffer, src->rawBuffer, src->rawBufferLengthBytes);
	}
}

int ltn_pes_packet_is_audio(struct ltn_pes_packet_s *pes)
{
	if ((pes->stream_id >= 0xc0) && (pes->stream_id <= 0xdf)) {
		return 1;
	}

	/* AC3 / private */
	if (pes->stream_id >= 0xfd) {
		return 1;
	}

	return 0;
}

int ltn_pes_packet_is_video(struct ltn_pes_packet_s *pes)
{
	if ((pes->stream_id >= 0xe0) && (pes->stream_id <= 0xef)) {
		return 1;
	}

	return 0;
}
