#include "libltntstools/nal.h"
#include "libltntstools/ts.h"
#include <inttypes.h>

#include <libavutil/internal.h>
#include <libavcodec/golomb.h>

int ltn_nal_findHeader(const uint8_t *buffer, int lengthBytes, int *offset)
{
	const uint8_t sig[] = { 0, 0, 1 };

	for (int i = (*offset + 1); i < lengthBytes - sizeof(sig); i++) {
		if (memcmp(buffer + i, sig, sizeof(sig)) == 0) {

			/* Check for the forbidden zero bit, it's illegal to be high in a nal (conflicts with PES headers. */
			if (*(buffer + i + 3) & 0x80)
				continue;

			*offset = i;
			return 0; /* Success */
		}
	}

	return -1; /* Not found */
}

static struct hevcNal_s {
	const char *name;
	const char *type;
} hevcNals[] = {
	[ 0] = { "TRAIL_N", .type = "AUTO" },
	[ 1] = { "TRAIL_R", .type = "IDR" },
	[ 2] = {   "TSA_N", .type = "I" },
	[ 3] = {   "TSA_R", .type = "P" },
	[ 4] = {  "STSA_N", .type = "BREF" },
	[ 5] = {  "STSA_R", .type = "B" },
	[ 6] = { "RADL_N" },
	[ 7] = { "RADL_R" },
	[ 8] = { "RASL_N" },
	[ 9] = { "RASL_R" },

	[19] = { "IDR_W_RADL", .type = "IDR" },
	[20] = { "IDR_N_LP" },
	[21] = { "CRA" },

	[32] = { "VPS" },
	[33] = { "SPS" },
	[34] = { "PPS" },
	[35] = { "AUD" },
	[36] = { "EOS" },
	[37] = { "EOB" },
	[38] = { "FD" },
	[39] = { "PREFIX_SEI" },
	[40] = { "SUFFIX_SEI" },
};

const char *hevcNals_lookupName(int nalType)
{
	return hevcNals[nalType].name;
}

const char *hevcNals_lookupType(int nalType)
{
	return hevcNals[nalType].type;
}

char *ltn_nal_hevc_findNalTypes(const uint8_t *buffer, int lengthBytes)
{
	char *arr = malloc(128);
	arr[0] = 0;

	int items = 0;
	int offset = -1;
	while (ltn_nal_findHeader(buffer, lengthBytes, &offset) == 0) {
		unsigned int nalType = (buffer[offset + 3] >> 1) & 0x3f;
		const char *nalName = hevcNals_lookupName(nalType);
		const char *nalTypeDesc = hevcNals_lookupType(nalType);

		if (items++ > 0)
			sprintf(arr + strlen(arr), ", ");

		sprintf(arr + strlen(arr), "%s", nalName);
#if 0
		printf("%6d: %02x %02x %02x %02x : type %2d (%s)\n",
			offset,
			buffer[offset + 0],
			buffer[offset + 1],
			buffer[offset + 2],
			buffer[offset + 3],
			nalType,
			nalName);
#endif
	}

	return arr;
}

static struct h264Nal_s {
	const char *name;
	const char *type;
} h264Nals[] = {
	[ 0] = { "UNSPECIFIED", .type = "AUTO" },
	[ 1] = { "P", .type = "P" },
	[ 2] = { "P", .type = "P" },
	[ 3] = { "P", .type = "P" },
	[ 4] = { "P", .type = "P" },
	[ 5] = { "IDR", .type = "IDR" },
	[ 6] = { "SEI" },
	[ 7] = { "SPS" },
	[ 8] = { "PPS" },
	[ 9] = { "AUD" },
	[10] = { "EO SEQ" },
	[11] = { "EO STREAM" },
	[12] = { "FILLER" },
	[13] = { "SPS-EX" },
	[14] = { "PNU" },
	[15] = { "SSPS" },
	[16] = { "DPS" },
	[19] = { "ACP" },
	[20] = { "CSE" },
	[21] = { "CSEDV" },
};

const char *h264Nals_lookupName(int nalType)
{
	return h264Nals[nalType].name;
}

const char *h264Nals_lookupType(int nalType)
{
	return h264Nals[nalType].type;
}

char *ltn_nal_h264_findNalTypes(const uint8_t *buffer, int lengthBytes)
{
	char *arr = calloc(1, 128);
	arr[0] = 0;

	int items = 0;
	int offset = -1;
	while (ltn_nal_findHeader(buffer, lengthBytes, &offset) == 0) {
		unsigned int nalType = buffer[offset + 3] & 0x1f;
		const char *nalName = h264Nals_lookupName(nalType);
		const char *nalTypeDesc = h264Nals_lookupType(nalType);

		if (items++ > 0)
			sprintf(arr + strlen(arr), ", ");

		sprintf(arr + strlen(arr), "%s", nalName);
#if 0
		printf("%6d: %02x %02x %02x %02x : type %2d (%s)\n",
			offset,
			buffer[offset + 0],
			buffer[offset + 1],
			buffer[offset + 2],
			buffer[offset + 3],
			nalType,
			nalName);
#endif
	}
	
	if (items == 0) {
		free(arr);
		return NULL;
	}

	return arr;
}


struct h264_slice_data_s
{
	uint32_t  slice_type;
	uint64_t  count;
	char     *name;
};

#define MAX_H264_SLICE_TYPES 10
struct h264_slice_data_s slice_defaults[MAX_H264_SLICE_TYPES] = {
	{ 0, 0, "P", },
	{ 1, 0, "B", },
	{ 2, 0, "I", },
	{ 3, 0, "SP", },
	{ 4, 0, "SI", },
	{ 5, 0, "P", },
	{ 6, 0, "B", },
	{ 7, 0, "I", },
	{ 8, 0, "SP", },
	{ 9, 0, "SI", },
};

struct h264_slice_counter_s
{
	uint16_t pid;
	struct h264_slice_data_s slice[MAX_H264_SLICE_TYPES];
};

void h264_slice_counter_reset(void *ctx)
{
	struct h264_slice_counter_s *s = (struct h264_slice_counter_s *)ctx;
	memcpy(s->slice, slice_defaults, sizeof(slice_defaults));
}

void *h264_slice_counter_alloc(uint16_t pid)
{
	struct h264_slice_counter_s *s = malloc(sizeof(*s));
	s->pid = pid;
	h264_slice_counter_reset(s);
	return (void *)s;
}

void h264_slice_counter_free(void *ctx)
{
	struct h264_slice_counter_s *s = (struct h264_slice_counter_s *)ctx;
	free(s);
}

void h264_slice_counter_update(void *ctx, int slice_type)
{
	struct h264_slice_counter_s *s = (struct h264_slice_counter_s *)ctx;
	s->slice[ slice_type ].count++;
}

void h264_slice_counter_dprintf(void *ctx, int fd, int printZeroCounts)
{
	struct h264_slice_counter_s *s = (struct h264_slice_counter_s *)ctx;
	dprintf(fd, "Type  Name  Count (H264 slice types for pid 0x%04x)\n", s->pid);
	for (int i = MAX_H264_SLICE_TYPES - 1; i >= 0 ; i--) {
		struct h264_slice_data_s *sl = &s->slice[i];
		if (sl->count == 0 && !printZeroCounts)
			continue;
		dprintf(fd, "%4d  %4s  %" PRIu64 "\n", sl->slice_type, sl->name, sl->count);
	}
}

static void h264_slice_counter_write_packet(void *ctx, const unsigned char *pkt)
{
	struct h264_slice_counter_s *s = (struct h264_slice_counter_s *)ctx;
	int offset = 0;
	while (offset < ((1 * 188) - 5)) {
		if (ltn_nal_findHeader(pkt, 188, &offset) == 0) {
#if 0
			printf("nal at 0x%04x: ", offset);
			for (int i = 0; i < 6; i++)
				printf("%02x ", *(pkts + offset + i));
#endif
			if ((*(pkt + offset + 3) & 0x1f) == 0x01) {
				GetBitContext gb;
				init_get_bits8(&gb, pkt + offset + 4, 4);
				get_ue_golomb(&gb); /* first_mb_in_slice */
				int slice_type = get_ue_golomb(&gb);

				h264_slice_counter_update(s, slice_type);
				//h264_slice_counter_dprintf(s, 0, 0);
			}
#if 0
			printf("\n");
#endif
		} else
			break;
	}
}

void h264_slice_counter_write(void *ctx, const unsigned char *pkts, int pktCount)
{
	struct h264_slice_counter_s *s = (struct h264_slice_counter_s *)ctx;
	for (int i = 0; i < pktCount; i++) {
		uint16_t pid = ltntstools_pid(pkts + (i * 188));
		if (s->pid == 0x2000 || pid == s->pid) {
			h264_slice_counter_write_packet(s, pkts + (i * 188));
		}
	}
}
