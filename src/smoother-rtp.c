/* Copyright LiveTimeNet, Inc. 2021. All Rights Reserved. */

#include <unistd.h>

#include "libltntstools/ltntstools.h"
#include "xorg-list.h"

#define LOCAL_DEBUG 0

struct smoother_rtp_item_s
{
	struct xorg_list list;
	uint64_t       seqno; /* Unique number per item, so we can check for loss/corruption in the lists. */

	unsigned char *buf;
	int            lengthBytes;
	int            maxLengthBytes;

	int            tsComputed; /* Boolean. Was the Timestamp in this item computed from a base offset, or read from stream? */

	struct ltntstools_pcr_position_s pcrdata; /* PCR value from pid N in the buffer, first PCR only. */
	uint64_t       received_TSuS;  /* Item received timestamp Via makeTimestampFromNow */
	uint64_t       scheduled_TSuS; /* Time this item is schedule for push via thread for smoothing output. */

	int            pcrDidReset; /* Boolean */
};

static void itemPrint(struct smoother_rtp_item_s *item)
{
	printf("seqno %" PRIu64, item->seqno);
	printf(" lengthBytes %5d", item->lengthBytes);
	printf(" received_TSuS %" PRIu64, item->received_TSuS);
	printf(" scheduled_TSuS %" PRIu64, item->scheduled_TSuS);
	printf(" tsComputed %d", item->tsComputed);
	printf(" pcr %" PRIi64 "  pcrDidReset %d\n", item->pcrdata.pcr, item->pcrDidReset);
}

/* TODO: Share this with the PCR smoother */
/* byte_array.... ---------- */
struct byte_array_s
{
	uint8_t *buf;
	int maxLengthBytes;
	int lengthBytes;
};

static int byte_array_init(struct byte_array_s *ba, int lengthBytes)
{
	ba->buf = malloc(lengthBytes);
	if (!ba->buf)
		return -1;

	ba->maxLengthBytes = lengthBytes;
	ba->lengthBytes = 0;

	return 0;
}

static void byte_array_free(struct byte_array_s *ba)
{
	free(ba->buf);
	ba->lengthBytes = 0;
	ba->maxLengthBytes = 0;
}

static int byte_array_append(struct byte_array_s *ba, const uint8_t *buf, int lengthBytes)
{
	if (ba->lengthBytes + lengthBytes > ba->maxLengthBytes) {
		ba->buf = realloc(ba->buf, ba->lengthBytes + lengthBytes);
		ba->maxLengthBytes += lengthBytes;
	}
	memcpy(ba->buf + ba->lengthBytes, buf, lengthBytes);
	ba->lengthBytes += lengthBytes;

	return lengthBytes;
}

static void byte_array_trim(struct byte_array_s *ba, int lengthBytes)
{
	if (lengthBytes > ba->lengthBytes)
		return;

	memmove(ba->buf, ba->buf + lengthBytes, ba->lengthBytes - lengthBytes);
	ba->lengthBytes -= lengthBytes;
}

static const uint8_t *byte_array_addr(struct byte_array_s *ba)
{
	return ba->buf;
}
/* byte_array.... ---------- */

struct smoother_rtp_context_s
{
	struct xorg_list itemsFree;
	struct xorg_list itemsBusy;
	pthread_mutex_t listMutex;

	void *userContext;
	smoother_rtp_output_callback outputCb;

	uint64_t walltimeFirstTimestampuS; /* Reset this when the clock significantly leaps backwards */
	int64_t tsFirst; /* Reset this when the clock significantly leaps backwards, 90KHz */
	int64_t tsTail; /* Timestamp on the first list item, 90KHz */
	int64_t tsHead; /* Timestamp on the last list item, 90KHz */

	int latencyuS;
	uint64_t bitsReceivedSinceLastPCR;

	uint64_t seqno;
	uint64_t last_seqno;

	/* Autodetect the SSRC. */
	uint32_t voteCompleteSSRC;
	uint32_t expectedSSRC;
	uint32_t expectedCount; /* Number of times we attempted to detect */
	uint32_t expectedValidateConditions; /* When it exceeds 5, we've detected the SSRC */

	int itemLengthBytes; /* Should be a minimum of 12 + (7 * 188) */
	pthread_t threadId;
	int threadRunning, threadTerminate, threadTerminated;

	int64_t totalSizeBytes; /* Total number of bytes in the queue */

	/* A contigious chunk of ram containing transport packets, in order.
	 * starting with a transport packet containing a PCR on pid ctx->pcrPid
	 */
	struct byte_array_s ba;

	/* Handle the case where the PCR goes forward or back in time,
	 * in our case by more than 15 seconds.
	 * Flag an internal PCR reset and let the implementation recompute its clocks.
	 */
	int didTimestampReset;
	time_t lastTimestampResetTime;

	int64_t measuredLatencyMs; /* based on first and last PCRs in the list, how much latency do we have? */

	struct ltn_histogram_s *histReceive;
	struct ltn_histogram_s *histTransmit;
};

/* based on first received pcr, and first received walltime, compute a new walltime
 * for this new input pts.
 */
static uint64_t getScheduledOutputuS(struct smoother_rtp_context_s *ctx, int64_t tsValue)
{
	int64_t ticks = ltntstools_pts_diff(ctx->tsFirst, tsValue);
	uint64_t scheduledTimeuS = ctx->walltimeFirstTimestampuS + ((ticks * 300) / 27); /* Convert 90Khz to 27Mhz then to US */

	/* Add user defined latency */
	scheduledTimeuS += ctx->latencyuS;

	return scheduledTimeuS;
}

__inline__ uint64_t makeTimestampFromTimeval(struct timeval *ts)
{
	uint64_t t = ((int64_t)ts->tv_sec * 1000000LL) + ts->tv_usec;
	return t;
}

__inline__ uint64_t makeTimestampFromNow()
{
	struct timeval now;
	gettimeofday(&now, NULL);
	return makeTimestampFromTimeval(&now);
}

__inline__ uint64_t makeTimestampFrom1SecondAgo()
{
	struct timeval now;
	gettimeofday(&now, NULL);
	now.tv_sec--;
	return makeTimestampFromTimeval(&now);
}

__inline__ uint64_t makeTimestampFrom2SecondAgo()
{
	struct timeval now;
	gettimeofday(&now, NULL);
	now.tv_sec--;
	return makeTimestampFromTimeval(&now);
}

static void itemFree(struct smoother_rtp_item_s *item)
{
	if (item) {
		free(item->buf);
		free(item);
	}
}

static void itemReset(struct smoother_rtp_item_s *item)
{
	item->lengthBytes = 0;
	item->received_TSuS = 0;
	item->scheduled_TSuS = 0;
	item->tsComputed = 0;
	ltntstools_pcr_position_reset(&item->pcrdata);
}

static struct smoother_rtp_item_s *itemAlloc(int lengthBytes)
{
	struct smoother_rtp_item_s *item = calloc(1, sizeof(*item));
	if (!item)
		return NULL;

	item->buf = calloc(1, lengthBytes);
	if (!item->buf) {
		free(item);
		item = NULL;
	}
	item->maxLengthBytes = lengthBytes;
	itemReset(item);

	return item;
}

#if LOCAL_DEBUG
static void _queuePrintList(struct smoother_rtp_context_s *ctx, struct xorg_list *head, const char *name)
{
	int totalItems = 0;

	printf("Queue %s -->\n", name);
	struct smoother_rtp_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, head, list) {
		totalItems++;
		itemPrint(e);
	}
	printf("Queue End --> %d items\n", totalItems);
}
#endif

void smoother_rtp_free(void *hdl)
{
	struct smoother_rtp_context_s *ctx = (struct smoother_rtp_context_s *)hdl;

	if (ctx->threadRunning) {
		ctx->threadTerminated = 0;
		ctx->threadTerminate = 1;
		while (!ctx->threadTerminated) {
			usleep(20 * 1000);
		}
	}

	pthread_mutex_lock(&ctx->listMutex);
	while (!xorg_list_is_empty(&ctx->itemsFree)) {
		struct smoother_rtp_item_s *item = xorg_list_first_entry(&ctx->itemsFree, struct smoother_rtp_item_s, list);
		xorg_list_del(&item->list);
		itemFree(item);
	}
	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_rtp_item_s *item = xorg_list_first_entry(&ctx->itemsBusy, struct smoother_rtp_item_s, list);
		xorg_list_del(&item->list);
		itemFree(item);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	byte_array_free(&ctx->ba);

	ltn_histogram_free(ctx->histReceive);
	ltn_histogram_free(ctx->histTransmit);

	free(ctx);
}

/*  Service the busy list. Find any items due for output
 *  and send via the callback.
 *  It's important that we hold the mutex for a short time so we don't block
 *  the _write() method.
 */
static int _queueProcess(struct smoother_rtp_context_s *ctx, int64_t uS)
{
	/* Take any node on the Busy list up to and including items with a timestamp of uS.
	 * Put them on a local list so we can free the holding mutex as fast as possible
	 */
	struct xorg_list loclist;
	xorg_list_init(&loclist);

	int count = 0, totalItems = 0, redundantItems = 0;
	struct smoother_rtp_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		totalItems++;

		if (totalItems == 1) {
			ctx->tsHead = e->pcrdata.pcr;
		}

		if (e->scheduled_TSuS <= uS) {
			xorg_list_del(&e->list);
			xorg_list_append(&e->list, &loclist);
			count++;
		} else {
			if (count > 0) {
				/* Time ordering problem on the list now. Dump both lists and abort, later.*/
				redundantItems++;
			}
		}
		/* TODO: The list is time ordered so we shoud be able to break
		 * when we find a time that's beyond out window, and save CPU time.
		 */
	}

	/* Make sure the busy list is contigious */
	e = NULL;
	next = NULL;
	int countSeq = 0;
	uint64_t last_seq = 0;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		countSeq++;

		if (countSeq > 1) {
			if (last_seq + 1 != e->seqno) {
				/* Almost certainly, the schedule US time is out of order, warn. */
				printf("List possibly mangled, seqnos might be bad now, %" PRIu64 ", %" PRIu64 "\n", last_seq, e->seqno);
#if LOCAL_DEBUG
				_queuePrintList(ctx, &ctx->itemsBusy, "Busy");
				_queuePrintList(ctx, &loclist, "loclist");
				fflush(stdout);
				fflush(stderr);
				exit(1);
#endif
			}
		}
		last_seq = e->seqno;
	}

	pthread_mutex_unlock(&ctx->listMutex);

	if (count <= 0) {
		return -1; /* Nothing scheduled, bail out early. */
	}

	/* Process the local list.
	 * Call the callback with any scheduled packets
	 */
	e = NULL, next = NULL;
	xorg_list_for_each_entry_safe(e, next, &loclist, list) {
		if (ctx->outputCb) {

			ltn_histogram_interval_update(ctx->histTransmit);

			ctx->outputCb(ctx->userContext, e->buf, e->lengthBytes);

			pthread_mutex_lock(&ctx->listMutex);			
			ctx->totalSizeBytes -= e->lengthBytes;
			pthread_mutex_unlock(&ctx->listMutex);

			/* Throw a packet loss warning if the queue gets confused, should never happen. */
			if (ctx->last_seqno && ctx->last_seqno + 1 != e->seqno) {
				printf("%s() seq err %" PRIu64 " vs %" PRIu64 "\n",__func__, ctx->last_seqno, e->seqno);
			}

			ctx->last_seqno = e->seqno;
		}
	}

	/* Take the mutex again to return the spent items to the free list */
	e = NULL, next = NULL;
	pthread_mutex_lock(&ctx->listMutex);
	xorg_list_for_each_entry_safe(e, next, &loclist, list) {
		itemReset(e);
		xorg_list_del(&e->list);
		xorg_list_append(&e->list, &ctx->itemsFree);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

extern int ltnpthread_setname_np(pthread_t thread, const char *name);

static void * _threadFunc(void *p)
{
	struct smoother_rtp_context_s *ctx = (struct smoother_rtp_context_s *)p;

	pthread_detach(ctx->threadId);
	ltnpthread_setname_np(ctx->threadId, "thread-rtpsmooth");

	ctx->threadTerminated = 0;
	ctx->threadRunning = 1;

	while (!ctx->threadTerminate) {
		pthread_mutex_lock(&ctx->listMutex);
		if (xorg_list_is_empty(&ctx->itemsBusy)) {
			pthread_mutex_unlock(&ctx->listMutex);
			usleep(1 * 1000);
			continue;
		}

		int64_t uS = makeTimestampFromNow();

		/* Service the output schedule queue, output any UDP packets when they're due.
		 * Important to remember that we're calling this func while we're holding the mutex.
		 */
		if (_queueProcess(ctx, uS) < 0)
			usleep(1 * 1000);

	}
	ctx->threadRunning = 1;
	ctx->threadTerminated = 1;

	/* TODO: pthread detach else we'll cause a small leak in valgrind. */
	return NULL;
}

int smoother_rtp_alloc(void **hdl, void *userContext, smoother_rtp_output_callback cb,
	int itemsPerSecond, int itemLengthBytes, int latencyMS)
{
	if (itemLengthBytes < 12 + (7 * 188)) {
		itemLengthBytes = 12 + (7 * 188);
	}

	struct smoother_rtp_context_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	xorg_list_init(&ctx->itemsFree);
	xorg_list_init(&ctx->itemsBusy);
	pthread_mutex_init(&ctx->listMutex, NULL);
	ctx->userContext = userContext;
	ctx->outputCb = cb;
	ctx->itemLengthBytes = itemLengthBytes;
	ctx->walltimeFirstTimestampuS = 0;
	ctx->tsFirst = -1;
	ctx->tsTail = -1;
	ctx->tsHead = -1;
	ctx->latencyuS = latencyMS * 1000;
	ctx->lastTimestampResetTime = time(NULL);
	byte_array_init(&ctx->ba, 8000 * 188); /* Initial size of 300mbps with 40ms PCR intervals */

	ltn_histogram_alloc_video_defaults(&ctx->histReceive, "receive arrival times");
	ltn_histogram_alloc_video_defaults(&ctx->histTransmit, "transmit arrival times");

	/* TODO: We probably don't need an itemspersecond fixed value, probably,
	 * calculate the number of items based on input bitrate value and
	 * a (TODO) future latency/smoothing window.
	 */
	pthread_mutex_lock(&ctx->listMutex);
	for (int i = 0; i < itemsPerSecond; i++) {
		struct smoother_rtp_item_s *item = itemAlloc(itemLengthBytes);
		if (!item)
			continue;
		xorg_list_append(&item->list, &ctx->itemsFree);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	/* Spawn a thread that manages the scheduled output queue. */
	pthread_create(&ctx->threadId, NULL, _threadFunc, ctx);

	*hdl = ctx;

	return 0;
}

int smoother_rtp_write2(void *hdl, const unsigned char *buf, int lengthBytes, int64_t tsValue)
{
	struct smoother_rtp_context_s *ctx = (struct smoother_rtp_context_s *)hdl;

	pthread_mutex_lock(&ctx->listMutex);
	if (xorg_list_is_empty(&ctx->itemsFree)) {
		/* Grow the free queue */
		for (int i = 0; i < 64; i++) {
			struct smoother_rtp_item_s *item = itemAlloc(ctx->itemLengthBytes);
			if (!item)
				continue;
			xorg_list_append(&item->list, &ctx->itemsFree);
		}
	}

	struct smoother_rtp_item_s *item = xorg_list_first_entry(&ctx->itemsFree, struct smoother_rtp_item_s, list);
	if (!item) {
		pthread_mutex_unlock(&ctx->listMutex);
		return -1;
	}

	xorg_list_del(&item->list);
	pthread_mutex_unlock(&ctx->listMutex);

	item->received_TSuS = makeTimestampFromNow();

	/* Grow the packet buffer if we really have to */
	if (item->maxLengthBytes < lengthBytes) {
		item->buf = realloc(item->buf, lengthBytes);
		item->maxLengthBytes = lengthBytes;
	}

	memcpy(item->buf, buf, lengthBytes);
	item->lengthBytes = lengthBytes;

	/* PCR found */
	item->pcrdata.pcr = tsValue;
	if (ctx->tsFirst == -1) {
#if LOCAL_DEBUG
		printf("ctx->tsFirst    was    %" PRIi64 ", ctx->walltimeFirstPCRuS %" PRIi64 "\n",
			ctx->tsFirst, ctx->walltimeFirstTimestampuS);
#endif
		ctx->tsFirst = item->pcrdata.pcr;
		ctx->walltimeFirstTimestampuS = item->received_TSuS;

#if LOCAL_DEBUG
		printf("ctx->tsFirst reset to %" PRIi64 ", ctx->walltimeFirstPCRuS %" PRIi64 "\n",
			ctx->tsFirst, ctx->walltimeFirstTimestampuS);
#endif
	}

	/* Reset number of packets received since the last PCR. */
	/* We use this along with an estimated input bitrate to calculated a sche duled output time. */
	ctx->bitsReceivedSinceLastPCR = 0;

	ctx->tsTail = item->pcrdata.pcr; /* Cache the last stream PCR */

	/* Figure out when this packet should be scheduled for output */
	item->scheduled_TSuS = getScheduledOutputuS(ctx, tsValue);
	item->tsComputed = 0;

	pthread_mutex_lock(&ctx->listMutex);
	item->seqno = ctx->seqno++;
	ctx->totalSizeBytes += item->lengthBytes;
	if (item->lengthBytes <= 0) {
		fprintf(stderr, "%s() bug, adding item with negative bytes %d\n", __func__, item->lengthBytes);
	}
#if 0
	itemPrint(item);
#endif

	/* Check the last item on the queue. scheduled time should never go backwards to
	 * to resampling the PCR timebase. never append an item with the scheduled time that goes
	 * backwards, eotherwise they're pulled off the queue in the wrong order very occasionally.
	 */
	if (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_rtp_item_s *last = xorg_list_last_entry(&ctx->itemsBusy, struct smoother_rtp_item_s, list);
		//struct smoother_rtp_item_s *last = ctx->itemsBusy.prev;
		if (last->scheduled_TSuS > item->scheduled_TSuS) {
			item->scheduled_TSuS = last->scheduled_TSuS + 1;
		}
	}

	/* Queue this for scheduled output */
	xorg_list_append(&item->list, &ctx->itemsBusy);
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

int smoother_rtp_write(void *hdl, const unsigned char *buf, int lengthBytes, struct timeval *ts)
{
	struct smoother_rtp_context_s *ctx = (struct smoother_rtp_context_s *)hdl;

	ltn_histogram_interval_update(ctx->histReceive);
#if LOCAL_DEBUG
	ltn_histogram_interval_print(STDOUT_FILENO, ctx->histReceive, 5);
	ltn_histogram_interval_print(STDOUT_FILENO, ctx->histTransmit, 5);
#endif

	if (ctx->voteCompleteSSRC == 0) {
		/* Ateme CM5000 likes to randomize its SSRC every time the stream starts.
		 * We use the SSRC (in part) to better match the locations of valid RTP
		 * header in random buffers. Take a vote on what the right SSRC should be
		 * on startup, then use it later to improve the frame filter.
		 */
		struct rtp_hdr *hdr = (struct rtp_hdr *)buf;
		ctx->expectedCount++;
		if (ctx->expectedValidateConditions == 0) {
			ctx->expectedSSRC = hdr->ssrc;
			ctx->expectedValidateConditions++;
		} else
		if (ctx->expectedSSRC == hdr->ssrc) {
			ctx->expectedValidateConditions++;
		}
		int iterations = 10;		
		if (ctx->expectedValidateConditions == iterations) {
			printf("smoother-rtp: After %d iterations of sampling, RTP smoother framework voted the SSRC to be 0x%x\n", iterations, ctx->expectedSSRC);
			ctx->voteCompleteSSRC = 1;
		}	
	}

	/* append all payload into a large buffer, allow fragmented writes */
	byte_array_append(&ctx->ba, buf, lengthBytes);

	/* Search this buffer for any rtp headers */
	struct rtp_frame_position_s *array = NULL;
	int arrayLength = 0;
	int r = rtp_frame_queryPositions(ctx->ba.buf, ctx->ba.lengthBytes, 0, ctx->expectedSSRC, &array, &arrayLength);
	if (r < 0)
		return 0;

	if (arrayLength < 2) {
		free(array);
		return 0;
	}

#if 0
	/* Amount of payload between the first two consecutive PCRs */
	int byteCount = (array[1]->offset - array[0]->offset);

	int pktCount = byteCount / 188;
	int64_t pcrIntervalPerPacketTicks = ltntstools_scr_diff(pcr[0]->pcr, pcr[1]->pcr) / pktCount;
#endif

#if 0
	for (int i = 0; i <arrayLength; i++) {
		printf("%d. offset %06" PRIu64 " length %06d\n", i, (array + i)->offset, (array + i)->lengthBytes);
	}
#endif

	/* Adjust network order bytes to simplify readabiloity */
	(array + 0)->frame.ts = ntohl((array + 0)->frame.ts);
	(array + 1)->frame.ts = ntohl((array + 1)->frame.ts);
#if 0
	printf("TS0: %10" PRIu32 " (%x) to TS1: %10" PRIu32 " (%x)\n",
		(array + 0)->frame.ts,
		(array + 0)->frame.ts,
		(array + 1)->frame.ts,
		(array + 1)->frame.ts);
#endif

	if (ltntstools_pts_diff((array + 0)->frame.ts, (array + 1)->frame.ts) > (15 * 90000)) {

		printf("smoother-rtp: Detected significant RTP timestamp jump from %" PRIu32 " to %" PRIu32 ":\n",
			(array + 0)->frame.ts,
			(array + 1)->frame.ts);

		if ((array + 0)->frame.ts < (array + 1)->frame.ts) {
			printf("  - time moves forwards\n");
			printf("  - b.ts = %14" PRIi32 ", %8" PRIu64 "\n", (array + 0)->frame.ts, (array + 0)->offset);
			printf("  - e.ts = %14" PRIi32 ", %8" PRIu64 "\n", (array + 1)->frame.ts, (array + 1)->offset);
		}
		if ((array + 0)->frame.ts > (array + 1)->frame.ts) {
			printf("  - time moves backwards\n");
			printf("  - b.ts = %14" PRIi32 ", %8" PRIu64 "\n", (array + 0)->frame.ts, (array + 0)->offset);
			printf("  - e.ts = %14" PRIi32 ", %8" PRIu64 "\n", (array + 1)->frame.ts, (array + 1)->offset);
		}

//		_queuePrintList(ctx, &ctx->itemsBusy, "Busy");

		ctx->didTimestampReset = 1;

		printf("Auto-correcting RTP timestamp schedule due to Timestamp timewrap\n");
	}

	ctx->measuredLatencyMs = ltntstools_pts_diff(ctx->tsHead, ctx->tsTail) / 90;
#if LOCAL_DEBUG
	{
		/* Dump the first and second PCR we found. */
		printf("tsHead %" PRIi64 " tsTail %" PRIi64 " latency %" PRIi64 " ms\n", ctx->tsHead, ctx->tsTail, ctx->measuredLatencyMs);
	}
#endif

	/* Find every RTP frame in the buffer UNTIL the last frame (which could be partial), and process it */
	uint32_t trimOffset = 0;
	for (int i = 0; i < arrayLength; i++ ) {
#if 0
		printf("Frame %d offset %" PRIu64 " length %d\n", i, (array + i)->offset, (array + i)->lengthBytes);
#endif
		if ((array + i)->lengthBytes == 0) {
//			printf("\taborting processing of 0 length frame\n");
			break;
		}
	
		/* TODO */
		/* TODO */
		/* TODO */
		/* TODO If we have PTS frame duplication across multiple RTP headers, we might need to smooth incrementally. */
		/* TODO */
		/* TODO */
		/* TODO */

		smoother_rtp_write2(ctx, &ctx->ba.buf[ (array + i)->offset ],
			(array + i)->lengthBytes,
			(array + i)->frame.ts);

		trimOffset = (array + i)->offset + (array + i)->lengthBytes;
	}

	/* Trim up to byte position X, to discard all of the rtp frames we processed in this iteration. */
	if (trimOffset != 1328) {
		printf("\ttrimming %d, maybe illegal\n", trimOffset);
	}
	byte_array_trim(&ctx->ba, trimOffset);
	
	free(array);

	/* If its been more than 60 seconds, reset the timestamp to avoid slow drift over time.
	 * Also, prevents issues where the tsFirst value wraps and tick calculations that
	 * drive scheduled packet output time goes back in time.
	 */
	time_t now = time(NULL);
	if (now >= ctx->lastTimestampResetTime + 60) {
		ctx->lastTimestampResetTime = now;
#if LOCAL_DEBUG
		printf("Triggering timestamp timebase recalculation\n");
#endif
		ctx->didTimestampReset = 1;
	}

	/* And finally, if during this pass we detected a timestamp reset, ensure the next
	 * round of writes resets its internal clocks and goes through a full timestamp reset.
	 */
	if (ctx->didTimestampReset) {
		ctx->tsFirst = -1;
		ctx->didTimestampReset = 0;
	}

	return 0;
}

int64_t smoother_rtp_get_size(void *hdl)
{
	struct smoother_rtp_context_s *ctx = (struct smoother_rtp_context_s *)hdl;
	int64_t sizeBytes = 0;

	pthread_mutex_lock(&ctx->listMutex);
	if (ctx->totalSizeBytes > 0)
		sizeBytes = ctx->totalSizeBytes;
	pthread_mutex_unlock(&ctx->listMutex);

	return sizeBytes;
}

void smoother_rtp_reset(void *hdl)
{
	struct smoother_rtp_context_s *ctx = (struct smoother_rtp_context_s *)hdl;

	pthread_mutex_lock(&ctx->listMutex);
	ctx->walltimeFirstTimestampuS = 0;
	ctx->tsFirst = -1;
	ctx->tsHead = -1;
	ctx->tsTail = -1;
	ctx->totalSizeBytes = 0;

	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_rtp_item_s *item = xorg_list_first_entry(&ctx->itemsBusy, struct smoother_rtp_item_s, list);
		itemReset(item);
		xorg_list_del(&item->list);
		xorg_list_append(&item->list, &ctx->itemsFree);
	}

	pthread_mutex_unlock(&ctx->listMutex);
}
