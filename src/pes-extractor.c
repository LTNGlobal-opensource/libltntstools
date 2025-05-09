#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libltntstools/ltntstools.h>
#include "klringbuffer.h"
#include "klbitstream_readwriter.h"

#define LOCAL_DEBUG 0
#define ORDERED_LIST_DEPTH 10
#define SIMULATE_TS_PACKET_LOSS 0

struct pes_extractor_s
{
	uint16_t pid;
	KLRingBuffer *rb;

	/* Valid states are:
	 * 0. Not appending packload from TS packets into a ring buffer.
	 * 1. when state == 0 and payload unit start indiactor arrives, go to state 1. Append bytes to ring buffer
	 * 2. when state == 1 and additional payload unit start indiactor arrives, append, processing ring and goto state 1.
	 */
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

	int computedRingSize; /* Amount of bytes we've written to the ring buffer */
	int largestRingFrame; /* Largest ever PES we've pulled from the ring buffer - useful for sizing */
	uint8_t lastCC;       /* Track CC loss for the pid and help prevent partial / mangles PES construction. */
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
	ctx->computedRingSize = 0;
	ctx->lastCC = 0xea;
	ctx->largestRingFrame = 0;
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

	//int cnt = 0;
	xorg_list_for_each_entry(e, &ctx->listOrdered, list) {
		//cnt++;
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

	//printf("%s() ctx->largestRingFrame largest size of a pes was %d bytes\n", __func__, ctx->largestRingFrame);
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

static int _processRing(struct pes_extractor_s *ctx)
{
	int rlen = rb_used(ctx->rb);
	if (rlen == 0) {
		return -1; /* Nothing to do */
	}

	if (ctx->computedRingSize != rlen) {
		printf("%s() %d vs %d, should never happen, aborting\n", __func__, ctx->computedRingSize, rlen);
		abort();
	}
	if (rlen < 16) {
		/* While technically possible, a PES is rarely less than
		 * 16 bytes so lets put some safely in place here.
		 */
		printf("%s() pes len %d < 16 bytes - should probably never happen, aborting\n", __func__, rlen);
		abort();
	}

	int overrun = 0;

#if LOCAL_DEBUG
	printf("%s() ring size %ld, computed size %d\n", __func__, rb_used(ctx->rb), ctx->computedRingSize);
#endif

	unsigned char *buf = malloc(rlen);
	if (buf) {
		int plen = rb_peek(ctx->rb, (char *)buf, rlen);
		if (plen == rlen) {

#if 0
			printf("A, plen %d -- first ", plen);
			for (int k = 0; k < 32; k++) {
				printf("%02x ", buf[k]);
			}
			printf("\n");
#endif

			/* Track a useful stat */
			if (plen > ctx->largestRingFrame) {
				ctx->largestRingFrame = plen;
			}

			struct klbs_context_s bs;
			klbs_init(&bs);
			klbs_read_set_buffer(&bs, buf, rlen);

			struct ltn_pes_packet_s *pes = ltn_pes_packet_alloc();
			int bitsProcessed = ltn_pes_packet_parse(pes, &bs, ctx->skipDataExtraction);

			/* check for buffer overrun */
			if (bs.overrun) {
				fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) Process Ring Buffer bs.overrun %d bs.buflen %d bs.buflen_used %d rlen %d\n",
						__FILE__, __func__, __LINE__, bs.overrun, bs.buflen, bs.buflen_used, rlen);
				ltn_pes_packet_dump(pes, "\t");
				overrun = 1;
			} else if (bs.truncated) {
				ltn_pes_packet_dump(pes, "\t");
			}

			if (!overrun && bitsProcessed && ctx->cb) {
				
				pes->rawBufferLengthBytes = rlen;
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

	if (overrun) {
		return -2;
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

#if SIMULATE_TS_PACKET_LOSS
		static uint64_t pidcount = 0;
		if (pidcount++ % 256 == 0) {
			/* Simulate packet loss on a pid */
			continue;
		}
#endif

		/* If we see a CC error on the pid we're extracting, restart the statemachine.
		 * Out rule is, we won't pass malformed PES's downstream to the caller.
		 */
		if (ctx->lastCC != 0xea && ltntstools_isCCInError(pkt, ctx->lastCC)) {
			printf("%s() detected pkt loss on pid 0x%04x had 0x%02x got 0x%02x\n", __func__,
				ctx->pid, ctx->lastCC, ltntstools_continuity_counter(pkt));

			/* Comment out this reset of you want to eventually send short
			 * malformed PES packets to the callbacks.
			 */
			ctx->appending = 0;
			rb_empty(ctx->rb);
			ctx->computedRingSize = 0;

		}
		ctx->lastCC = ltntstools_continuity_counter(pkt);

		/* We don't append packets to the ring until we've seen our first payload start indicator.
		 * even after a CC error on this pid.
		 */
		if (ltntstools_payload_unit_start_indicator(pkt) == 0 && ctx->appending == 0) {
			continue;
		}

		/* start indicator received, but we're not appending - yet */
		if (ltntstools_payload_unit_start_indicator(pkt) && ctx->appending == 0) {
			/* Reset the state machine */
			ctx->appending = 1;
			rb_empty(ctx->rb);
			ctx->computedRingSize = 0;
		}

		/* Skip any adaption stuffing */
		int offset = 4;
		if (ltntstools_has_adaption((uint8_t *)pkt)) {
			offset++;
			offset += ltntstools_adaption_field_length(pkt);
		}

		if (ltntstools_payload_unit_start_indicator(pkt) == 0 && ctx->appending == 1) {
			/* Continue appending the current packet into the pes, we're mid pes */

			int wsize = 188 - offset;
			ctx->computedRingSize += wsize;
			rb_write_with_state(ctx->rb, (const char *)pkt + offset, wsize, &didOverflow);

		} else
		if (ltntstools_payload_unit_start_indicator(pkt) == 1 && ctx->appending == 1) {

			/* See ISO13818-1 2000(E) - section 2.4.3.3.
			 * When the payload of the Transport Stream packet contains PES packet data,
			 * the payload_unit_start_indicator has the following significance: a '1' indicates that the
			 * payload of this Transport Stream packet will commence with the first byte
			 * of a PES packet and a '0' indicates no PES packet shall start in this
			 * Transport Stream packet. If the payload_unit_start_indicator is set to '1',
			 * then one and only one PES packet starts in this Transport Stream packet.
			 * This also applies to private streams of stream_type 6 (refer to Table 2-29).
			 */

			/* Process the ring, might be empty */
			int pr_ret = _processRing(ctx);

			/* Clean the ring */
			rb_empty(ctx->rb);
			ctx->computedRingSize = 0;

			/* Write new leading pes data into ring */
			int wsize = 188 - offset;
			ctx->computedRingSize += wsize;
			rb_write_with_state(ctx->rb, (const char *)pkt + offset, wsize, &didOverflow);
			if (didOverflow) {
#if 1
				printf("%s() overflow of ring, aborting\n", __func__);
				abort();
#endif
			}
		}
	}

	return packetCount;
}
