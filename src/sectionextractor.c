
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

#include "libltntstools/sectionextractor.h"
#include "libltntstools/ts.h"
#include "libltntstools/crc32.h"

struct sectionextractor_ctx_s
{
	uint8_t tableID;
	uint16_t PID;
	int complete;

	int appending;
	unsigned char *section;
	unsigned int sectionLength;
	unsigned int sectionLengthCurrent;
};

void ltntstools_sectionextractor_free(void *hdl)
{
	struct sectionextractor_ctx_s *ctx = (struct sectionextractor_ctx_s *)hdl;
	free(ctx->section);
	free(ctx);
}

int ltntstools_sectionextractor_alloc(void **hdl, uint16_t PID, uint8_t tableID)
{
	struct sectionextractor_ctx_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->tableID = tableID;
	ctx->PID = PID;
	ctx->section = malloc(4096);
	*hdl = ctx;
	return 0;
}

static ssize_t ltntstools_sectionextractor_write_packet(struct sectionextractor_ctx_s *ctx,
	const uint8_t *pkt, int *complete, int *crcValid)
{
	int section_offset = 4;
	*crcValid = 0;

	/* Limitations. The entire section has to fit within a single packet. */

#if 0
	printf("packet:\n");
	for (int i = 1; i <= 188; i++) {
		printf("%02x ", *(pkt + i - 1));
		if (i % 16 == 0)
			printf("\n");
	}
	printf("\n");
#endif
	/* Some basic sanity. */
	if (*(pkt + 0) != 0x47)
		return 0;

	/* If the packet contains adaption, it appears before the table data,
	 * adjust our offset to skip the adaption data.
	 */
	if (ltntstools_has_adaption((unsigned char *)pkt)) {
		section_offset++;
		section_offset += ltntstools_adaption_field_length((unsigned char *)pkt);
	}

	/* If the Packet Marker is set, this packet contains the start of the table_section,
	 * and it could span multiple packets.
	 * The start table is preceeded by a pointer_field, which in turn could shift the
	 * position of the first table_section byte EVEN FURTHER into the packet, accomodate
	 * a pointer_field.
	 */
	if (ltntstools_payload_unit_start_indicator(pkt)) {
		/* Prior to the table_section we have a pointer field, consider this */
		section_offset += *(pkt + section_offset);
		section_offset++;

		/* Safety */
		if (section_offset >= 180)
			return 0;
	}

	int copylength = 0;
	if (ctx->appending == 0 && ctx->complete == 0 && *(pkt + section_offset) == ctx->tableID) {
		ctx->appending = 1;
		ctx->complete = 0;
		ctx->sectionLengthCurrent = 0;
		ctx->sectionLength = (*(pkt + section_offset + 1) << 8 | *(pkt + section_offset + 2)) & 0xfff;
		ctx->sectionLength += 3;
		copylength = ctx->sectionLength;
		if (copylength > 183)
			copylength = 183;
#if 0
		printf("section TID 0x%02x starts offset 0x%02x length 0x%x appending %d\n",
			*(pkt + section_offset),
			section_offset, ctx->sectionLength, ctx->appending);
#endif
	} else
	if (ctx->appending == 1 && ctx->complete == 0) {
		copylength = 183;
		if (ctx->sectionLengthCurrent + copylength > ctx->sectionLength) {
			copylength = ctx->sectionLength - ctx->sectionLengthCurrent;
		}
	} else {
		ctx->complete = 0;
		ctx->appending = 0;
		return -1;
	}

#if 0
	printf("section appending 0x%x bytes from offset 0x%x\n", copylength, section_offset);
#endif

#if 0
	for (int i = 1; i <= 188; i++) {
		printf("%02x ", *(pkt + i - 1));
		if (i % 16 == 0)
			printf("\n");
	}
	printf("\n");
#endif

#if 0
	printf("Section 0x%02x length 0x%02x/0x%x bytes\n",
		ctx->tableID, ctx->sectionLengthCurrent + copylength,
		ctx->sectionLength);
#endif
	memcpy(&ctx->section[ctx->sectionLengthCurrent], pkt + section_offset, copylength);
	ctx->sectionLengthCurrent += copylength;

	if (ctx->sectionLength == ctx->sectionLengthCurrent) {

		if (ltntstools_checkCRC32(ctx->section, ctx->sectionLength + 3) == 0) {
			/* CRC is correct. */
			*crcValid = 1;
		} else {
			*crcValid = 0;
		}

		ctx->complete = 1;
		ctx->appending = 0;
		*complete = 1;
	}

	return copylength;
}

ssize_t ltntstools_sectionextractor_write(void *hdl, const uint8_t *pkt, size_t packetCount, int *complete, int *crcValid)
{
	struct sectionextractor_ctx_s *ctx = (struct sectionextractor_ctx_s *)hdl;

	*complete = 0;

	ssize_t ret = 0;
	for (int i = 0; i < packetCount; i++) {
		if (ltntstools_pid(&pkt[i * 188]) != ctx->PID)
			continue;
		ret += ltntstools_sectionextractor_write_packet(ctx, &pkt[i * 188], complete, crcValid);
	}

	if (*complete)
		ctx->complete = 1;

	return ret;
}

int ltntstools_sectionextractor_query(void *hdl, uint8_t *dst, int lengthBytes)
{
	struct sectionextractor_ctx_s *ctx = (struct sectionextractor_ctx_s *)hdl;

	if (!ctx->complete || !dst || lengthBytes < (ctx->sectionLength + 3))
		return -1;

	memcpy(dst, &ctx->section[0], ctx->sectionLength + 3);

	ctx->complete = 0;
	ctx->appending = 0;
	return ctx->sectionLength + 3;
}
