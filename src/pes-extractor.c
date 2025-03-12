#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libltntstools/ltntstools.h>
#include "klringbuffer.h"
#include "klbitstream_readwriter.h"
#include "memmem.h"

#define LOCAL_DEBUG 0
#define ORDERED_LIST_DEPTH 10

struct pes_extractor_s
{
	uint16_t pid;
	KLRingBuffer *rb;
	int appending;
	uint8_t streamId;
	int skipDataExtraction;

	pes_extractor_callback cb;
	void *userContext;

	/* Cache N pes packets and emit them in PTS order,
	 * where the lowest PTS PES from an array is emitted
	 * when a new PES is added.
	 *
	 */
	int orderedOutput;
	struct xorg_list listOrdered;
	pthread_mutex_t listOrderedMutex;
	int64_t orderedBaseTime;
	int64_t lastDeliveredPTS;
};

struct item_s
{
	struct xorg_list list;
	int64_t correctedPTS; /* true 64bit number where when the PTS wrapper we don't truncate, always increasing value. */
	struct ltn_pes_packet_s *pes;
};

int ltntstools_pes_extractor_alloc(void **hdl, uint16_t pid, uint8_t streamId, pes_extractor_callback cb, void *userContext, int buffer_min, int buffer_max)
{
	struct pes_extractor_s *ctx = calloc(1, sizeof(*ctx));

	if (buffer_min == -1)
		buffer_min = 4 * 1048576;
	if (buffer_max == -1)
		buffer_max = 32 * 1048576;

	ctx->rb = rb_new(buffer_min, buffer_max);
	ctx->pid = pid;
	ctx->streamId = streamId;
	ctx->cb = cb;
	ctx->userContext = userContext;
	ctx->skipDataExtraction = 0;
	ctx->orderedOutput = 0;
	ctx->orderedBaseTime = 0;
	xorg_list_init(&ctx->listOrdered);
	pthread_mutex_init(&ctx->listOrderedMutex, NULL);

	/* initialize a 10 item deep list */
	for (int i = 0; i < ORDERED_LIST_DEPTH; i++) {
		struct item_s *item = malloc(sizeof(*item));
		if (item) {
			item->correctedPTS = 0;
			item->pes = NULL;
			xorg_list_append(&item->list, &ctx->listOrdered);
		} 
	}

	*hdl = ctx;
	return 0;
}

#if LOCAL_DEBUG
static void _list_print(struct pes_extractor_s *ctx)
{
	struct item_s *e = NULL;

	int n = 0;
	xorg_list_for_each_entry(e, &ctx->listOrdered, list) {
		printf("item[%2d] %p correctedPTS %" PRIi64 "\n", n++, e, e->correctedPTS);
	}
}
#endif

static void _list_insert(struct pes_extractor_s *ctx, struct item_s *newitem)
{
	struct item_s *e = NULL;

	int didAdd = 0;
	xorg_list_for_each_entry(e, &ctx->listOrdered, list) {
		if (newitem->correctedPTS < e->correctedPTS) {
			__xorg_list_add(&newitem->list, e->list.prev, &e->list);
			didAdd++;
			break;
		}
		if (e->pes == NULL) {
			__xorg_list_add(&newitem->list, e->list.prev, &e->list);
			didAdd++;
			break;
		}
	}
	if (didAdd == 0) {
		xorg_list_append(&newitem->list, &ctx->listOrdered);
	}
}

static struct item_s * _list_find_oldest(struct pes_extractor_s *ctx)
{
	struct item_s *e = NULL;
	struct item_s *oldest = NULL;

	int cnt = 0;
	xorg_list_for_each_entry(e, &ctx->listOrdered, list) {
		cnt++;
		if (oldest == NULL) {
			oldest = e;
		} else {
			if (e->correctedPTS < oldest->correctedPTS) {
				oldest = e;
			}
		}
	}
	return oldest;
}

void ltntstools_pes_extractor_free(void *hdl)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	rb_free(ctx->rb);

	while (!xorg_list_is_empty(&ctx->listOrdered)) {
		struct item_s *item = xorg_list_first_entry(&ctx->listOrdered, struct item_s, list);
		if (item->pes) {
			ltn_pes_packet_free(item->pes);
			item->pes = NULL;
			item->correctedPTS = 0;
		}
		xorg_list_del(&item->list);
		free(item);
	}

	free(ctx);
}

int ltntstools_pes_extractor_set_ordered_output(void *hdl, int tf)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	ctx->orderedOutput = tf;
	return 0; /* Success */
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
	unsigned char pattern[4] = {0x00, 0x00, 0x01, ctx->streamId};
	int rlen = rb_used(ctx->rb);
	if (rlen < 4)
		return;
	size_t trimmed = 0;
	uint8_t buf[1024];	// Buffer for peeking data
	size_t overlap = 3; // Overlap to handle pattern spanning chunks
	while (rlen >= 4)
	{
		// Determine how much to read in this iteration
		size_t toRead = (rlen > sizeof(buf)) ? sizeof(buf) : rlen;
		size_t len = rb_peek(ctx->rb, (char *)buf, toRead);
		if (len < 4)
			break;
		// Search for the pattern in the current buffer using memmem
		const void *pos = ltn_memmem(buf, len, pattern, sizeof(pattern));
		if (pos)
		{
			// Pattern found, calculate offset and discard up to pattern
			size_t index = (const uint8_t *)pos - buf;
			rb_discard(ctx->rb, index);
			trimmed += index;
			break;
		}
		// If pattern not found, discard up to overlap size to preserve possible pattern start
		size_t toDiscard = (len > overlap) ? (len - overlap) : len;
		rb_discard(ctx->rb, toDiscard);
		trimmed += toDiscard;
		rlen = rb_used(ctx->rb); // Update remaining data size
	}
}

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)(p[0]) 
         | ((uint32_t)(p[1]) << 8)
         | ((uint32_t)(p[2]) << 16)
         | ((uint32_t)(p[3]) << 24);
}

static int searchReverse(const unsigned char *buf, int lengthBytes, uint8_t streamId)
{
    /* Construct the 32-bit pattern [00 00 01 streamId] in memory order. 
     * For little-endian, that's (streamId << 24) + 0x010000.
     */
    uint32_t pattern = ((uint32_t)streamId << 24) | ((uint32_t)0x01 << 16);

    for (int i = lengthBytes - 4; i >= 0; i--)
    {
        /* Load 4 bytes from buf[i..i+3] as one 32-bit little-endian value. */
        uint32_t val = read_u32_le(&buf[i]);
        if (val == pattern) {
            return i;
        }
    }

    return -1;
}

static int _processRing(struct pes_extractor_s *ctx)
{
	int rlen = rb_used(ctx->rb);
	if (rlen < 16)
		return -1;
	int overrun = 0;

#if LOCAL_DEBUG
	printf("%s() ring size %d\n", __func__, rb_used(ctx->rb));
#endif

	//fprintf(stderr, "_processRing (%s:%s:%d) rlen %d\n", __FILE__, __func__, __LINE__, rlen);

	unsigned char *buf = malloc(rlen);
	if (buf) {
		int plen = rb_peek(ctx->rb, (char *)buf, rlen);
		if (plen == rlen) {
			/* Search backwards for the start of the next mpeg signature.
			 * result is the position of the signature as an offset from the beginning of the buffer.
			 * If the value is zero, we only havea  single porbably incomplete PES in the buffer, which is
			 * meaningless, becasue the buffer is expected to contain and ENTIRE PES followed by the header from
			 * a subsequence PES.
			 */
			int offset = searchReverse(buf, rlen, ctx->streamId);
			if (offset < 16) {
				/* We'll come back again in the future */
				free(buf);
				return -1;
			}

			/*
			 * If this is private_stream_1 (0xbd) and the declared PES length is bigger
			 * than the available bytes in the ring, then override the declared length
			 * to zero so the parser won't read beyond the buffer (avoiding overrun).
			 */
			if ((ctx->streamId == 0xBD || (ctx->streamId >= 0xC0 && ctx->streamId <= 0xDF)) && (offset + 6 <= rlen))
			{
				uint16_t declaredLen = (buf[offset + 4] << 8) | buf[offset + 5];
				size_t have = (rlen - offset);
				size_t needed = declaredLen + 6;

				if (needed > have)
				{
					// Start after PES header
					int last_complete_frame = offset + 6;
					int found_frame = 0;

					// For PES private stream 1 (0xBD) - typically AC3
					if (ctx->streamId == 0xBD)
					{
						// Search for AC3 sync words (0x0B77)
						for (int i = offset + 6; i < rlen - 2; i++)
						{
							if (buf[i] == 0x0B && buf[i + 1] == 0x77)
							{
								last_complete_frame = i;
								found_frame = 1;
							}
						}
					}
					// For MPEG audio streams (0xC0-0xDF) - MP2/AAC
					else if (ctx->streamId >= 0xC0 && ctx->streamId <= 0xDF)
					{
						// Search for MPEG audio sync words (0xFFF* for AAC, 0xFFF* or 0xFFE* for MP2)
						for (int i = offset + 6; i < rlen - 2; i++)
						{
							if (buf[i] == 0xFF && ((buf[i + 1] & 0xF0) == 0xF0 || (buf[i + 1] & 0xF0) == 0xE0))
							{
								last_complete_frame = i;
								found_frame = 1;
							}
						}
					}

					if (found_frame)
					{
						// Adjust length to include only complete frames
						uint16_t adjustedLen = last_complete_frame - (offset + 6);
						if (adjustedLen > 0)
						{
							buf[offset + 4] = (adjustedLen >> 8) & 0xFF;
							buf[offset + 5] = adjustedLen & 0xFF;
						}
					}
					else
					{
						// If we can't find any frames, just process header
						buf[offset + 4] = 0x00;
						buf[offset + 5] = 0x00;
					}
				}
			}
#if LOCAL_DEBUG
			if (offset == 423)
			{
				ltntstools_hexdump(buf, rlen, 32);
			}
			printf("%s() offset %d, rlen %d\n", __func__, offset, rlen);
#endif
			struct klbs_context_s bs;
			klbs_init(&bs);
			klbs_read_set_buffer(&bs, buf, rlen - (rlen - offset)); /* This ensures the entire PES payload is collected */
#if LOCAL_DEBUG
			printf("%s() set bs length to %d bytes\n", __func__, rlen - (rlen - offset));
#endif
			struct ltn_pes_packet_s *pes = ltn_pes_packet_alloc();
			//ssize_t xlen =
			int bitsProcessed = ltn_pes_packet_parse(pes, &bs, ctx->skipDataExtraction);

			/* check for buffer overrun */
			if (bs.overrun) {
#if KLBITSTREAM_DEBUG
				fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) Process Ring Buffer bs.overrun %d bs.buflen %d bs.buflen_used %d rlen %d offset %d\n",
						__FILE__, __func__, __LINE__, bs.overrun, bs.buflen, bs.buflen_used, rlen, offset);
#endif
#if KTBITSTREAM_DUMP_ON_OVERRUN
				ltn_pes_packet_dump(pes, "\t");
#endif
#if KLBITSTREAM_RETURN_ON_OVERRUN
				ltn_pes_packet_free(pes);
				free(buf);
				return -2;
#else
				overrun = 1;
#endif
			} else if (bs.truncated) {
#if KTBITSTREAM_DUMP_ON_OVERRUN
				ltn_pes_packet_dump(pes, "\t");
#endif
			}

			if (!overrun && bitsProcessed && ctx->cb) {
				
				pes->rawBufferLengthBytes = rlen - (rlen - offset);
				pes->rawBuffer = malloc(pes->rawBufferLengthBytes);
				memcpy(pes->rawBuffer, buf, pes->rawBufferLengthBytes);

				if (ctx->orderedOutput) {
					/* Send the PES's to the callback in the correct temporal order,
					 * which compensates for B frames.
					 */
					struct item_s *item = _list_find_oldest(ctx);
					if (item) {
						if (item->pes) {
							/* User owns the lifetime of the object */
							ctx->cb(ctx->userContext, item->pes);
						}
						item->pes = pes;
						if ((pes->PTS + (10 * 90000)) < ctx->lastDeliveredPTS) {
							/* PTS has wrapped. Increment our base so we continue to order the 
							 * list correctly, regardless.
							 */
							ctx->orderedBaseTime += MAX_PTS_VALUE;
						}
						item->correctedPTS = ctx->orderedBaseTime + pes->PTS; /* TODO: handle the wrap */

						/* Now put the current parsed item on the list for future callback */
						xorg_list_del(&item->list);
						_list_insert(ctx, item);
#if LOCAL_DEBUG
						_list_print(ctx);
#endif
						ctx->lastDeliveredPTS = pes->PTS;
					}

				} else {
					ctx->cb(ctx->userContext, pes);
					/* User owns the lifetime of the object */
				}
			} else
			if (bitsProcessed) {
#if LOCAL_DEBUG
				ltn_pes_packet_dump(pes, "\t");
				ltn_pes_packet_free(pes);
#endif
			} else {
#if LOCAL_DEBUG
				printf("skipping, processedbits = %d\n", bitsProcessed);
#endif
			}
		}
		free(buf);
	}

	uint8_t tbuf[16];
	size_t l = rb_read(ctx->rb, (char *)&tbuf[0], sizeof(tbuf));
	if (l == 16) {
		//ltntstools_hexdump(buf, sizeof(tbuf), 16);
	}

#if LOCAL_DEBUG
	printf("%s() ring processing complete, size now %d\n", __func__, rb_used(ctx->rb));
#endif

	if (overrun) {
		return -2;
	}

	return 0; /* Success */
}

ssize_t ltntstools_pes_extractor_write(void *hdl, const uint8_t *pkts, int packetCount)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;

	int didOverflow;
#if KLBITSTREAM_RETURN_ON_OVERRUN
	int overrun = 0;
#endif
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
			//_trimRing(ctx);
			int pr_ret = _processRing(ctx);
			if (pr_ret == -2) { /* buffer overrun */
#if KLBITSTREAM_DEBUG
				fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) Pes Extractor Write buffer overrun in _processRing() for pid 0x%04x pkt size %d offset %d didOverflow %d\n",
						__FILE__, __func__, __LINE__, ctx->pid, 188, offset, didOverflow);
#endif
#if KLBITSTREAM_RETURN_ON_OVERRUN
				overrun = 1;
#endif
#if KLBITSTREAM_RESET_ON_OVERRUN
				ctx->appending = 0;
				_trimRing(ctx);
				rb_empty(ctx->rb);
				break;
#endif
			}
			else if (pr_ret == -1)
			{
				/* need more data */
				ctx->appending = 1;
				continue;
			}
			ctx->appending = 1;

			/* Now flush the buffer up to the next pes header marker */
			if (pr_ret == 0) {
				_trimRing(ctx);
			}
		}

	}

#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (overrun) {
		return -1;
	}
#endif

	return packetCount;
}
