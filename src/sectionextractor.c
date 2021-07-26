
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

struct sectionextractor_ctx_s
{
	uint8_t tableID;
	uint16_t PID;
	int complete;

	unsigned char section[188];
	unsigned int sectionLength;
};

void ltntstools_sectionextractor_free(void *hdl)
{
	struct sectionextractor_ctx_s *ctx = (struct sectionextractor_ctx_s *)hdl;
	free(ctx);
}

int ltntstools_sectionextractor_alloc(void **hdl, uint16_t PID, uint8_t tableID)
{
	struct sectionextractor_ctx_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->tableID = tableID;
	ctx->PID = PID;
	*hdl = ctx;
	return 0;
}

static ssize_t ltntstools_sectionextractor_write_packet(struct sectionextractor_ctx_s *ctx,
	const uint8_t *pkt, int *complete)
{
	int section_offset = 4;

	/* Limitations. The entire section has to fit within a single packet. */

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

#if 0
	printf("section offset 0x%02x\n", section_offset);
	printf("section offset TID 0x%02x\n", *(pkt + section_offset));
#endif
	if (*(pkt + section_offset) != ctx->tableID)
		return 0;
#if 0
	for (int i = 1; i <= 188; i++) {
		printf("%02x ", *(pkt + i - 1));
		if (i % 16 == 0)
			printf("\n");
	}
	printf("\n");
#endif
	ctx->sectionLength = (*(pkt + section_offset + 1) << 8 | *(pkt + section_offset + 2)) & 0xfff;

#if 0
	printf("Section 0x%02x length 0x%02x bytes\n", ctx->tableID, ctx->sectionLength);
#endif
	memcpy(&ctx->section[0], pkt + section_offset, ctx->sectionLength + 3);

	ctx->complete = 1;
	*complete = 1;

	return ctx->sectionLength + 3;
}

ssize_t ltntstools_sectionextractor_write(void *hdl, const uint8_t *pkt, size_t packetCount, int *complete)
{
	struct sectionextractor_ctx_s *ctx = (struct sectionextractor_ctx_s *)hdl;

	*complete = 0;
	ctx->complete = 0;

	ssize_t ret = 0;
	for (int i = 0; i < packetCount; i++) {
		if (ltntstools_pid(&pkt[i * 188]) != ctx->PID)
			continue;
		ret += ltntstools_sectionextractor_write_packet(ctx, &pkt[i * 188], complete);
	}

	if (*complete)
		ctx->complete = 1;

	return ret;
}

int ltntstools_sectionextractor_query(void *hdl, uint8_t *dst, int lengthBytes)
{
	struct sectionextractor_ctx_s *ctx = (struct sectionextractor_ctx_s *)hdl;

	if (!ctx->complete)
		return -1;

	memcpy(dst, &ctx->section[0], ctx->sectionLength + 3);

	return ctx->sectionLength + 3;
}

