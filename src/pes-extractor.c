#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libltntstools/ltntstools.h>
#include "klringbuffer.h"
#include "libltntstools/klbitstream_readwriter.h"
#include "utils.h"

#define LOCAL_DEBUG 0
#define ORDERED_LIST_DEPTH 60
#define SIMULATE_TS_PACKET_LOSS 0

struct pcr_item_s
{
	struct xorg_list list;
	int64_t pcr;
	uint32_t ringPos;
	time_t updateTime;
};
#define MAX_PCR_ITEMS 12

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

	struct ltntstools_corrected_clock_s correctedClock;

	int computedRingSize; /* Amount of bytes we've written to the ring buffer */
	int largestRingFrame; /* Largest ever PES we've pulled from the ring buffer - useful for sizing */
	uint64_t lastCCCounter; /* Track CC loss for the pid and help prevent partial / mangles PES construction. */

	struct ltntstools_stream_statistics_s *libstats;

	/* PCR to ring position management */
	struct xorg_list pcrList;
	uint32_t pusi_time_ms; /* Arrival duration of the entire pes */

	/* Blocks write() from touching resources that free() is tearing down.
	 * Atomic so the flag itself can be safely set (by free()) and read (by
	 * write()) from different threads without a data race. This narrows
	 * the race window around a free() call but does not make write() and
	 * free() safe to run fully concurrently on the same handle: a write()
	 * that already passed this check before free() sets it can still be
	 * using ctx->rb while free() releases it. Callers must still not call
	 * write() and free() on the same handle from different threads without
	 * their own external synchronization.
	 */
	int preventWrites;
};

struct item_s
{
	struct xorg_list list;
	int64_t correctedPTS; /* true 64bit number where when the PTS wrapper we don't truncate, always increasing value. */
	struct ltn_pes_packet_s *pes;
};

/* Matches ltntstools_notification_callback's real signature exactly (void
 * return, void *userContext first param -- stats.h:147). Previously
 * declared returning void* and taking struct pes_extractor_s * directly,
 * registered via an explicit cast below. Calling through a function
 * pointer of a mismatched type is undefined behavior per the C standard --
 * stats.c does exactly that, unconditionally, on every PUSI packet (see
 * EVENT_UPDATE_PID_PUSI_DELIVERY_TIME dispatch). Harmless on real calling
 * conventions, but not guaranteed. Same class of bug, and same fix, as
 * demux_pid_pe_callback's for pes_extractor_callback.
 */
static void notification_callback(void *userContext, enum ltntstools_notification_event_e event,
	const struct ltntstools_stream_statistics_s *stats,
	const struct ltntstools_pid_statistics_s *pid)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)userContext;

	if (event == EVENT_UPDATE_PID_PUSI_DELIVERY_TIME) {
		ctx->pusi_time_ms = pid->pusi_time_ms;
	}
}

/* Return codes:
 *   0  Success.
 *  -1  hdl is NULL.
 *  -2  buffer_min or buffer_max is negative and not exactly -1 (the only
 *      valid "use default" sentinel), or rb_new() itself rejected the
 *      resulting sizes (buffer_min > buffer_max, or either is exactly 0).
 *      rb_new() doesn't distinguish "bad size relationship" from "malloc()
 *      itself failed" in its own return value, so a real OOM on the ring
 *      buffer's backing allocation also surfaces as this code -- see the
 *      logged message for the size arguments actually used.
 *  -3  calloc() of the context itself failed (OOM).
 *  -4  ltntstools_pid_stats_alloc() failed (OOM). This framework's
 *      documented guarantee -- packet loss (CC errors) on the chosen pid
 *      is tracked and corrupted streams discarded -- depends entirely on
 *      this subsystem, so its failure fails the whole construction rather
 *      than returning a handle that can never detect loss.
 *  -5  Could not fully populate the ORDERED_LIST_DEPTH-deep ordered-output
 *      cache list (OOM).
 *  -6  Could not fully populate the MAX_PCR_ITEMS-deep PCR cache list
 *      (OOM). A totally empty pcrList would otherwise leave
 *      updatePcrList() to operate on an empty list later.
 */
int ltntstools_pes_extractor_alloc(void **hdl, uint16_t pid, uint8_t streamId, pes_extractor_callback cb, void *userContext, int buffer_min, int buffer_max)
{
	if (!hdl) {
		/* Consistent with every other handle-based entry point in this
		 * file (free(), set_ordered_output(), set_skip_data(),
		 * set_pcr_pid(), write()) -- this was the one left unchecked
		 * before dereferencing at `*hdl = ctx;` below.
		 */
		return -1;
	}

	if (buffer_min < -1 || buffer_max < -1) {
		/* -1 is the sole sentinel for "use the default". Any other
		 * negative value would otherwise wrap to a huge unsigned size_t
		 * once handed to rb_new() below, relying on the resulting
		 * malloc() failure to fail safe rather than rejecting bad input
		 * explicitly.
		 */
		fprintf(stderr, "%s() buffer_min %d / buffer_max %d: negative and not -1, rejecting\n",
			__func__, buffer_min, buffer_max);
		return -2;
	}

	struct pes_extractor_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		fprintf(stderr, "%s() calloc() of the context failed (OOM)\n", __func__);
		return -3;
	}

	/* buffer_min is eagerly malloc()'d by rb_new() below, right now, for
	 * every context -- unlike buffer_max, which is just a lazy-growth
	 * ceiling that costs nothing until a PES actually needs it. Growth
	 * happens in generous steps (partly a function of a single TS
	 * packet's payload contribution, see rb_write_with_state()) and,
	 * once grown, a ring's allocation is never shrunk back down between
	 * PES frames (rb_empty() only resets fill/head, not the allocation
	 * size) -- so a small start mainly costs a handful of regrows near
	 * the start of a stream, settling at whatever size the pid's real
	 * frames need, not a per-frame tax. That's what makes a small default
	 * safe across pid types (tiny audio/ancillary frames never grow past
	 * this at all; video frames climb to their steady-state size once).
	 */
	if (buffer_min == -1)
		buffer_min = 2 * 1024;
	if (buffer_max == -1)
		buffer_max = 4 * 1048576;

	ctx->rb = rb_new(buffer_min, buffer_max);
	if (!ctx->rb) {
		fprintf(stderr, "%s() rb_new(%d, %d) failed (invalid size relationship, or OOM)\n",
			__func__, buffer_min, buffer_max);
		free(ctx);
		return -2;
	}
	ctx->pid = pid;
	ctx->streamId = streamId;
	ctx->cb = cb;
	ctx->userContext = userContext;
	ctx->skipDataExtraction = 0;
	ctx->orderedOutput = 0;
	ctx->computedRingSize = 0;
	ctx->lastCCCounter = 0;
	ctx->largestRingFrame = 0;
	ctx->preventWrites = 0;
	ltntstools_corrected_clock_init(&ctx->correctedClock, 90000);
	xorg_list_init(&ctx->pcrList);
	xorg_list_init(&ctx->listOrdered);
	pthread_mutex_init(&ctx->listOrderedMutex, NULL);
	if (ltntstools_pid_stats_alloc(&ctx->libstats) < 0) {
		/* This framework's documented guarantee -- packet loss (CC errors)
		 * on the chosen pid is tracked and corrupted streams discarded --
		 * depends entirely on libstats. Treat its failure the same as any
		 * other construction failure (eg. rb_new() above) rather than
		 * silently returning a handle that can never detect loss. ctx is
		 * in a state free() already knows how to tear down safely (rb
		 * allocated, both lists initialized-but-empty, libstats NULL and
		 * every teardown call on it already NULL-safe), so reuse it
		 * instead of duplicating cleanup here.
		 */
		fprintf(stderr, "%s() ltntstools_pid_stats_alloc() failed (OOM)\n", __func__);
		ltntstools_pes_extractor_free(ctx);
		return -4;
	}
	ltntstools_notification_register_callback(ctx->libstats, EVENT_UPDATE_PID_PUSI_DELIVERY_TIME,
		ctx, notification_callback);

	/* initialize a 10 item deep list */
	for (int i = 0; i < ORDERED_LIST_DEPTH; i++) {
		struct item_s *item = malloc(sizeof(*item));
		if (!item) {
			/* Partial failure here previously went unreported: alloc()
			 * still returned success with a short (or, on total OOM,
			 * empty) listOrdered, silently degrading ordered-output mode
			 * for the life of the context. Fail the whole construction
			 * instead -- same rationale as the libstats check above.
			 */
			fprintf(stderr, "%s() ordered-output cache list malloc() failed at item %d/%d (OOM)\n",
				__func__, i, ORDERED_LIST_DEPTH);
			ltntstools_pes_extractor_free(ctx);
			return -5;
		}
		item->correctedPTS = 0;
		item->pes = NULL;
		xorg_list_append(&item->list, &ctx->listOrdered);
	}

	for (int i = 0; i < MAX_PCR_ITEMS; i++) {
		struct pcr_item_s *e = malloc(sizeof(*e));
		if (!e) {
			/* Same rationale as the listOrdered loop above. A totally
			 * empty pcrList here would also leave updatePcrList() to
			 * operate on an empty list later -- failing fast here avoids
			 * that state ever being reachable through this API.
			 */
			fprintf(stderr, "%s() PCR cache list malloc() failed at item %d/%d (OOM)\n",
				__func__, i, MAX_PCR_ITEMS);
			ltntstools_pes_extractor_free(ctx);
			return -6;
		}
		e->updateTime = time(0);
		e->pcr = 0;
		e->ringPos = 0;
		xorg_list_append(&e->list, &ctx->pcrList);
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

static void updatePcrList(struct pes_extractor_s *ctx, int64_t pcr, unsigned int ringPos)
{
	if (xorg_list_is_empty(&ctx->pcrList)) {
		/* xorg_list_last_entry() on an empty list computes a struct
		 * pcr_item_s* via container_of() applied to the list head itself
		 * (which lives inside struct pes_extractor_s, not a real
		 * pcr_item_s allocation) -- the `if (item)` check that used to
		 * guard this can never catch that, since the macro can't return
		 * NULL. Guard explicitly instead. Not reachable through the
		 * public API today (ltntstools_pes_extractor_alloc() now fails
		 * the whole construction if it can't fully populate pcrList), but
		 * this function shouldn't depend on that invariant holding forever.
		 */
		return;
	}

	/* Take the oldtest item, update and push to top of list. */
	struct pcr_item_s *item = xorg_list_last_entry(&ctx->pcrList, struct pcr_item_s, list);
	xorg_list_del(&item->list);
	item->pcr = pcr;

	/* Remember the next ring insert point (its tail) */
	item->ringPos = ringPos;

	item->updateTime = time(0);
	xorg_list_add(&item->list, &ctx->pcrList);

#if LOCAL_DEBUG
	printPcrList(ctx);
#endif
}

#if LOCAL_DEBUG
void printPcrItem(struct pes_extractor_s *ctx, int nr, struct pcr_item_s *e)
{
	printf("%2d: pcr %14" PRIi64 " pos %8d time %12d\n", nr, e->pcr, e->ringPos, (int)e->updateTime);
}

void printPcrList(struct pes_extractor_s *ctx)
{
	int i = 0;
	struct pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->pcrList, list) {
		printPcrItem(ctx, i++, e);
	}
}
#endif

/* For a given position in the ring buffer, find it's actual or synthesized PCR */
int64_t findPcrFromPosition(struct pes_extractor_s *ctx, unsigned int ringPos)
{
	struct pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->pcrList, list) {
		if (e->ringPos == ringPos) {
			return e->pcr;
		}
	}

	return -1;
}

/* Called exclusively from ltntstools_pes_extractor_free().
 * We're shutting down, flush any cached PES's otherwise we'll short
 * any tools any important ES data.
 * Only applicable when the caller has initialize this framework
 * with ltntstools_pes_extractor_set_ordered_output(ctx, TRUE);
 */
void _flushOrderedOutput(struct pes_extractor_s *ctx)
{
	/* Send the PES's to the callback in the correct temporal order,
	 * which compensates for B frames. Delete the list item because
	 * we're in the process of shutting down.
	 */
	struct item_s *item = _list_find_oldest(ctx);
	while (item) {
		if (item->pes) {
			/* User owns the lifetime of the object */
			ctx->cb(ctx->userContext, item->pes);
		}

		//ctx->lastDeliveredPTS = item->pes->PTS;

		xorg_list_del(&item->list);
		item->pes = NULL;
		item->correctedPTS = 0;
		free(item);

		item = _list_find_oldest(ctx);
	}
}

void ltntstools_pes_extractor_free(void *hdl)
{
	if (!hdl) {
		return;
	}

	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;

	/* Block any write() call that checks this flag from touching ctx->rb
	 * (freed right below) or other resources torn down in this function.
	 * This was previously (and backwards) `= 0`, which left the guard in
	 * ltntstools_pes_extractor_write() permanently dead -- it was never
	 * set to 1 anywhere in this file. See the preventWrites field comment
	 * for what this atomic store does and does not guarantee.
	 */
	__atomic_store_n(&ctx->preventWrites, 1, __ATOMIC_SEQ_CST);

	rb_free(ctx->rb);

	if (ctx->orderedOutput) {
		_flushOrderedOutput(ctx);
	}

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

	while (!xorg_list_is_empty(&ctx->pcrList)) {
		struct pcr_item_s *item = xorg_list_first_entry(&ctx->pcrList, struct pcr_item_s, list);
		xorg_list_del(&item->list);
		free(item);
	}

	ltntstools_notification_unregister_callbacks(ctx->libstats);
	ltntstools_pid_stats_free(ctx->libstats);

	//printf("%s() ctx->largestRingFrame largest size of a pes was %d bytes\n", __func__, ctx->largestRingFrame);
	free(ctx);
}

int ltntstools_pes_extractor_set_ordered_output(void *hdl, int tf)
{
	if (!hdl) {
		return -1;
	}

	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	ctx->orderedOutput = tf;
	return 0; /* Success */
}

int ltntstools_pes_extractor_set_skip_data(void *hdl, int tf)
{
	if (!hdl) {
		return -1;
	}

	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	ctx->skipDataExtraction = tf;
	return 0; /* Success */
}

/* Return codes:
 *    0  The ring was peeked and parsed without hitting any of the error
 *       conditions below. Note this does NOT guarantee a PES was delivered
 *       to the callback -- eg. ltn_pes_packet_parse() processing zero bits
 *       (a truncated/unparseable PES) also returns 0 here; that's a
 *       separate, not-yet-addressed gap in this return code's meaning.
 *   -1  Ring was empty, nothing to do.
 *   -2  Bitstream reader overran the ring content.
 *   -3  Internal computedRingSize/rb_used bookkeeping mismatch.
 *   -4  Ring content shorter than a PES could plausibly be (< 16 bytes).
 *   -5  malloc() of the ring peek buffer failed (OOM); a PES was dropped.
 *   -6  ltn_pes_packet_alloc() failed (OOM); a PES was dropped.
 */
static int _processRing(struct pes_extractor_s *ctx)
{
	int rlen = rb_used(ctx->rb);
	if (rlen == 0) {
		return -1; /* Nothing to do */
	}

	if (ctx->computedRingSize != rlen) {
		/* Internal bookkeeping drift between computedRingSize and the ring's
		 * actual used byte count -- should never happen, but killing the
		 * entire host process on it is a disproportionate response for a
		 * library. Discard this ring's content and let the caller (which
		 * unconditionally empties the ring right after calling this
		 * function) recover on the next PES boundary instead.
		 */
		fprintf(stderr, "%s() computedRingSize %d vs rb_used %d, should never happen, discarding ring\n",
			__func__, ctx->computedRingSize, rlen);
		return -3;
	}
	if (rlen < 16) {
		/* While technically possible, a PES is rarely less than
		 * 16 bytes so lets put some safety in place here. A malformed or
		 * truncated stream can plausibly hit this, so discard and return
		 * an error rather than aborting the whole process.
		 */
		fprintf(stderr, "%s() pes len %d < 16 bytes - should probably never happen, discarding ring\n",
			__func__, rlen);
		return -4;
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
			if (!pes) {
				free(buf);
				/* Distinct from the -1 "nothing to do" case above: this is
				 * a real dropped-PES error (OOM), not a benign empty ring.
				 */
				return -6;
			}
			int bitsProcessed = ltn_pes_packet_parse(pes, &bs, ctx->skipDataExtraction);

			pes->pcr = findPcrFromPosition(ctx, rb_get_read_pos(ctx->rb));
			if (pes->pcr == -1) {
				fprintf(stderr, "%s() this should never happen, pcr was negative\n", __func__);
			}
			pes->arrivalMs = ctx->pusi_time_ms;

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

				/* buf has already been fully consumed by
				 * ltn_pes_packet_parse() above (it deep-copies whatever it
				 * needs into pes->data separately), so nothing else needs
				 * it -- hand its ownership straight to pes->rawBuffer
				 * instead of a second malloc()+memcpy() of up to rlen
				 * (buffer_max, 32MB by default) bytes. buf is set to NULL
				 * so the free(buf) below becomes a no-op for this path;
				 * ltn_pes_packet_free() now owns and will free it.
				 */
				pes->rawBufferLengthBytes = rlen;
				pes->rawBuffer = buf;
				buf = NULL;

				if (ctx->orderedOutput) {
					/* Send the PES's to the callback in the correct temporal order,
					 * which compensates for B frames. IN other words, we've just built
					 * a pes above, but this might not be the right PES to emit to the callback,
					 * we're trying to emit an earlier PES to maintain temporal order.
					 * Find the oldest, calback that, and put the NEW pes we've just created in the
					 * right place in the time ordered queue, for later emmission.
					 */
					struct item_s *item = _list_find_oldest(ctx);
					if (item) {
						if (item->pes) {
							/* User owns the lifetime of the object */
							ctx->cb(ctx->userContext, item->pes);
						}

						/* Now re-use list item to store the newly constructed pes, put it back in the sorted list */
						item->pes = pes;
						ltntstools_corrected_clock_update(&ctx->correctedClock, pes->PTS);

						/* Get a PTS value that includes continious wrapping over time. */
						item->correctedPTS = ltntstools_corrected_clock_unwrapped(&ctx->correctedClock);

						/* Now put the current parsed item on the list for future callback */
						xorg_list_del(&item->list);
						_list_insert(ctx, item);
#if LOCAL_DEBUG
						_list_print(ctx);
#endif
					} else {
						/* listOrdered is empty (eg. every item malloc() failed
						 * during alloc()) -- nowhere to cache pes, so it would
						 * otherwise leak here.
						 */
						ltn_pes_packet_free(pes);
					}

				} else {
					ctx->cb(ctx->userContext, pes);
					/* User owns the lifetime of the object */
				}
			} else
			if (bitsProcessed) {
				/* Either bs.overrun or ctx->cb == NULL -- either way, pes
				 * was never delivered or cached, so it must be freed here
				 * or it leaks.
				 */
#if LOCAL_DEBUG
				ltn_pes_packet_dump(pes, "\t");
#endif
				ltn_pes_packet_free(pes);
			} else {
				/* ltn_pes_packet_parse() processed nothing (eg. truncated
				 * ring content) -- pes is still a live allocation and must
				 * be freed here or it leaks.
				 */
#if LOCAL_DEBUG
				printf("skipping, processedbits = %d\n", bitsProcessed);
#endif
				ltn_pes_packet_free(pes);
			}
		}
		free(buf);
	} else {
		/* malloc() failure must not be reported as success -- nothing was
		 * processed, so a PES on this ring was silently dropped.
		 */
		return -5;
	}

	if (overrun) {
		return -2;
	}

	return 0; /* Success */
}

int ltntstools_pes_extractor_set_pcr_pid(void *hdl, uint16_t pcrpidnr)
{
	if (!hdl) {
		return -1;
	}

	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	ltntstools_pid_stats_pid_set_contains_pcr(ctx->libstats, pcrpidnr & 0x1fff);

	return 0; /* Success */
}

ssize_t ltntstools_pes_extractor_write(void *hdl, const uint8_t *pkts, int packetCount)
{
	if (!hdl || (!pkts && packetCount > 0)) {
		/* Distinct from the -1 "closing down" rejection below: this is a
		 * caller error (bad arguments), not a valid handle being torn down.
		 */
		return -2;
	}

	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;

	int didOverflow;

	if (__atomic_load_n(&ctx->preventWrites, __ATOMIC_SEQ_CST)) {
		/* Library closing down. 0 is also a legitimate "processed zero
		 * matching packets" result below, so this must be distinguishable
		 * from that -- use a negative value, consistent with this file's
		 * other error returns.
		 */
		return -1; /* Failed */
	}

	int pidMatchedCount = 0;

	for (int i = 0; i < packetCount; i++) {
		const uint8_t *pkt = pkts + (i * 188);

		/* Specifically fcall this per packet, not per buffer, for better STC generation.*/
		ltntstools_pid_stats_update(ctx->libstats, pkt, 1);

		if (ltntstools_pid(pkt) != ctx->pid)
			continue;

		pidMatchedCount++;

#if SIMULATE_TS_PACKET_LOSS
		static uint64_t pidcount = 0;
		if (pidcount++ % 256 == 0) {
			/* Simulate packet loss on a pid */
			continue;
		}
#endif

		/* If this pid has a scr */
		/* Reset timebase to current pcr */
		/* if it doesn't have a pcr, increment local stc by one packets worth of ticks. */

		/* If we see a CC error on the pid we're extracting, restart the statemachine.
		 * Out rule is, we won't pass malformed PES's downstream to the caller.
		 */
		uint64_t c = ltntstools_pid_stats_stream_get_cc_errors(ctx->libstats);
		if (ctx->lastCCCounter != c) {
			printf("%s() detected pkt loss on pid 0x%04x had %" PRIu64 " now %" PRIu64 "\n",
				__func__,
				ctx->pid, ctx->lastCCCounter, c);

			/* Comment out this reset of you want to eventually send short
			 * malformed PES packets to the callbacks.
			 */
			ctx->appending = 0;
			rb_empty(ctx->rb);
			ctx->computedRingSize = 0;

		}
		ctx->lastCCCounter = c;

		if (ltntstools_adaption_field_control(pkt) == 0x02 /* Adaption only */) {
			continue;
		}

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
		if (offset > 188) {
			/* adaptation_field_length is an untrusted, attacker-controlled
			 * byte (0-255): a corrupt or malicious packet can claim more
			 * adaptation space than the 188-byte packet actually has.
			 * There's no payload left to extract in that case -- clamp
			 * instead of letting `188 - offset` go negative, which becomes
			 * a huge size_t once handed to rb_write_with_state() below.
			 */
			offset = 188;
		}

		if (ltntstools_payload_unit_start_indicator(pkt) == 0 && ctx->appending == 1) {
			/* Continue appending the current packet into the pes, we're mid pes */

			int wsize = 188 - offset;
			ctx->computedRingSize += wsize;
			rb_write_with_state(ctx->rb, (const char *)pkt + offset, wsize, &didOverflow);
			if (didOverflow) {
				/* rb_write_with_state() has already safely discarded old
				 * ring content to make room -- not fatal, but this PES's
				 * payload is now corrupted. _processRing()'s bs.overrun/
				 * bs.truncated checks are the real safety net: they'll
				 * detect and reject a corrupted PES rather than deliver
				 * a mangled one to the callback.
				 */
				fprintf(stderr, "%s() ring buffer overflow on pid 0x%04x (buffer_max reached), PES will be discarded\n",
					__func__, ctx->pid);
			}

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
			_processRing(ctx);

			/* Clean the ring */
			rb_empty(ctx->rb);
			ctx->computedRingSize = 0;

			if (1) {
				int64_t pcr;
				ltntstools_bitrate_calculator_query_stc(ctx->libstats, &pcr);

				/* The need to put rongpos and pcr on a list might be redundant, during
				 * testing the ring pos was always zero for any pcr.
				 * However, when the PCR isn't on the PES then we'll need this mechanism,
				 * so, keep it for now - future improvement.
				 */
				updatePcrList(ctx, pcr, rb_get_write_pos(ctx->rb));
			}

			/* Write new leading pes data into ring */
			int wsize = 188 - offset;
			ctx->computedRingSize += wsize;
			rb_write_with_state(ctx->rb, (const char *)pkt + offset, wsize, &didOverflow);
			if (didOverflow) {
				/* Same rationale as the mid-PES overflow check above --
				 * not fatal, _processRing()'s overrun/truncation checks
				 * are the real safety net that will reject the resulting
				 * corrupted PES rather than deliver it to the callback.
				 * Previously called abort() here, killing the entire host
				 * process over a single oversized/malformed PES.
				 */
				fprintf(stderr, "%s() ring buffer overflow on pid 0x%04x (buffer_max reached), PES will be discarded\n",
					__func__, ctx->pid);
			}
		}
	}

	return pidMatchedCount;
}
