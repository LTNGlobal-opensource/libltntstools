#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libltntstools/ltntstools.h>
#include "klringbuffer.h"
#include "klbitstream_readwriter.h"

#define LOCAL_DEBUG 0

struct pes_extractor_s
{
	uint16_t pid;
	KLRingBuffer *rb;
	int appending;
	uint8_t streamId;
	int skipDataExtraction;

	pes_extractor_callback cb;
	void *userContext;
};

int ltntstools_pes_extractor_alloc(void **hdl, uint16_t pid, uint8_t streamId, pes_extractor_callback cb, void *userContext)
{
	struct pes_extractor_s *ctx = calloc(1, sizeof(*ctx));

	ctx->rb = rb_new(4 * 1048576, 32 * 1048576);
	ctx->pid = pid;
	ctx->streamId = streamId;
	ctx->cb = cb;
	ctx->userContext = userContext;
	ctx->skipDataExtraction = 0;

	*hdl = ctx;
	return 0;
}

void ltntstools_pes_extractor_free(void *hdl)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	rb_free(ctx->rb);
	free(ctx);
}

int ltntstools_pes_extractor_set_skip_data(void *hdl, int tf)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	ctx->skipDataExtraction = tf;
	return 0; /* Success */
}

/* Remove any bytes leading up to a 00 00 01 pattern, align the ring.  */
static void _trimRing(struct pes_extractor_s *ctx)
{
	unsigned char pattern[4] = { 0x00, 0x00, 0x01, ctx->streamId };

	int rlen = rb_used(ctx->rb);
	if (rlen <= 0)
		return;

	int count = 0;
	uint8_t buf[8];

	while (1) {
		size_t l = rb_peek(ctx->rb, (char *)&buf[0], 4);
		if (l != 4)
			break;

		if (memcmp(pattern, buf, 4) != 0) {
			rb_discard(ctx->rb, 1);
			count++;
		} else
			break;
	}
}

static int searchReverse(const unsigned char *buf, int lengthBytes, uint8_t streamId)
{
	unsigned char pattern[4] = { 0x00, 0x00, 0x01, streamId };

	for (int i = lengthBytes - 4; i >= 0; i--) {
		if (memcmp(pattern, buf + i, 4) == 0)
			return i;
	}

	return -1;
}

static int _processRing(struct pes_extractor_s *ctx)
{
	//printf("%s() ring size %d\n", __func__, rb_used(ctx->rb));

	int rlen = rb_used(ctx->rb);
	if (rlen < 16)
		return -1;

	unsigned char *buf = malloc(rlen);
	if (buf) {
		int plen = rb_peek(ctx->rb, (char *)buf, rlen);
		if (plen == rlen) {
			/* Search backwards for the start of the next mpeg signature */
			int offset = searchReverse(buf, rlen, ctx->streamId);

			struct klbs_context_s bs;
			klbs_init(&bs);
			klbs_read_set_buffer(&bs, buf, rlen - (rlen - offset));

			struct ltn_pes_packet_s *pes = ltn_pes_packet_alloc();
			//ssize_t xlen =
			ltn_pes_packet_parse(pes, &bs, ctx->skipDataExtraction);

			if (ctx->cb) {
				
				pes->rawBufferLengthBytes = rlen - (rlen - offset);
				pes->rawBuffer = malloc(pes->rawBufferLengthBytes);
				memcpy(pes->rawBuffer, buf, pes->rawBufferLengthBytes);

				ctx->cb(ctx->userContext, pes);
				/* User owns the lifetime of the object */
			} else {
				ltn_pes_packet_dump(pes, "\t");
				ltn_pes_packet_free(pes);
			}
		}
		free(buf);
	}

	uint8_t tbuf[16];
	size_t l = rb_read(ctx->rb, (char *)&tbuf[0], sizeof(tbuf));
	if (l == 16) {
		//ltntstools_hexdump(buf, sizeof(tbuf), 16);
	}

	return 0; /* Success */
}

ssize_t ltntstools_pes_extractor_write(void *hdl, const uint8_t *pkts, int packetCount)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;

	int didOverflow;
	for (int i = 0; i < packetCount; i++) {
		const uint8_t *pkt = pkts + (i * 188);
		if (ltntstools_pid(pkt) != ctx->pid)
			continue;

		int offset = 4;
		/* Skip any adaption stuffing */
		if (ltntstools_has_adaption((uint8_t *)pkt)) {
			offset++;
			offset += ltntstools_adaption_field_length(pkt);
		}

		if (ltntstools_payload_unit_start_indicator(pkt) && ctx->appending == 1) {
			ctx->appending = 2;
		}
		if (ltntstools_payload_unit_start_indicator(pkt) && ctx->appending == 0) {
			ctx->appending = 1;
		}

		if (ctx->appending) {
			rb_write_with_state(ctx->rb, (const char *)pkt + offset, 188 - offset, &didOverflow);
		}

		if (ltntstools_payload_unit_start_indicator(pkt) && ctx->appending == 2) {
			/* Process any existing data in the ring. */
			_trimRing(ctx);
			_processRing(ctx);
			ctx->appending = 1;

			/* Now flush the buffer up to the next pes header marker */
			_trimRing(ctx);
		}

	}


	return packetCount;
}
