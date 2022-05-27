/* Copyright LiveTimeNet, Inc. 2021. All Rights Reserved. */

#include <unistd.h>

#include "libltntstools/ltntstools.h"
#include "xorg-list.h"

struct smoother_pcr_item_s
{
	struct xorg_list list;
	uint64_t seqno; /* Unique number per item, so we can check for loss/corruption in the lists. */

	unsigned char *buf;
	int lengthBytes;
	int maxLengthBytes;

	int pcrComputed; /* Boolean. Was the PCR in this item computed from a base offset, or read from stream? */

	struct ltntstools_pcr_position_s pcrdata; /* PCR value from pid N in the buffer, first PCR only. */
	uint64_t received_TSuS;  /* Item received timestamp Via makeTimestampFromNow */
	uint64_t scheduled_TSuS; /* Time this item is schedule for push via thread for smoothing output. */
};

void itemPrint(struct smoother_pcr_item_s *item)
{
	printf("seqno %" PRIu64, item->seqno);
	printf(" lengthBytes %d", item->lengthBytes);
	printf(" received_TSuS %" PRIu64, item->received_TSuS);
	printf(" scheduled_TSuS %" PRIu64, item->scheduled_TSuS);
	printf(" pcrComputed %d", item->pcrComputed);
	printf(" pcr %" PRIi64 "\n", item->pcrdata.pcr);
}

struct smoother_pcr_context_s
{
	struct xorg_list itemsFree;
	struct xorg_list itemsBusy;
	pthread_mutex_t listMutex;

	void *userContext;
	smoother_pcr_output_callback outputCb;

	uint64_t walltimeFirstPCRuS;
	int64_t pcrFirst;
	int64_t pcrLast;
	uint16_t pcrPID;

	int inputMuxrate_bps;
	uint64_t bitsReceivedSinceLastPCR;

	uint64_t seqno;
	uint64_t last_seqno;

	int itemLengthBytes;
	pthread_t threadId;
	int threadRunning, threadTerminate, threadTerminated;

	int64_t totalSizeBytes;
};

/* based on last received PCR, and number of bytes received since then,
 * and a notional input bitrate, calculate the current PCR.
 */
static int64_t getPCR(struct smoother_pcr_context_s *ctx, int additionalBits)
{
	return ctx->pcrLast + ((((double)ctx->bitsReceivedSinceLastPCR +
		(double)additionalBits) / (double)ctx->inputMuxrate_bps) * (double)27000000);
}

/* based on first received pcr, and first received walltime, compute a new walltime
 * for this new input pcr.
 */
static uint64_t getScheduledOutputuS(struct smoother_pcr_context_s *ctx, int64_t pcr)
{
	int64_t ticks = pcr - ctx->pcrFirst;
	uint64_t scheduledTimeuS = ctx->walltimeFirstPCRuS + (ticks / 27);

	/* Add 1 second of latency */
	scheduledTimeuS += 1000000;

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

static void itemFree(struct smoother_pcr_item_s *item)
{
	if (item) {
		free(item->buf);
		free(item);
	}
}

static void itemReset(struct smoother_pcr_item_s *item)
{
	item->lengthBytes = 0;
	item->received_TSuS = 0;
	item->scheduled_TSuS = 0;
	item->pcrComputed = 0;
	ltntstools_pcr_position_reset(&item->pcrdata);
}

static struct smoother_pcr_item_s *itemAlloc(int lengthBytes)
{
	struct smoother_pcr_item_s *item = calloc(1, sizeof(*item));
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

void smoother_pcr_free(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	if (ctx->threadRunning) {
		ctx->threadTerminated = 0;
		ctx->threadTerminate = 1;
		while (!ctx->threadTerminated) {
			usleep(20 * 1000);
		}
	}

	pthread_mutex_lock(&ctx->listMutex);
	while (!xorg_list_is_empty(&ctx->itemsFree)) {
		struct smoother_pcr_item_s *item = xorg_list_first_entry(&ctx->itemsFree, struct smoother_pcr_item_s, list);
		xorg_list_del(&item->list);
		itemFree(item);
	}
	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_pcr_item_s *item = xorg_list_first_entry(&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		xorg_list_del(&item->list);
		itemFree(item);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	free(ctx);
}

/*  Service the busy list. Find any items due for output
 *  and send via the callback.
 *  It's important that we hole the mutex for a short time so we don't block
 *  the _write() method.
 */
static int _queueProcess(struct smoother_pcr_context_s *ctx, int64_t uS)
{
	/* Take anything on the Busy up to and including items
	 * with a timestamp of uS
	 * Put them on a local list so we can free the holding mutex as fast as possible
	 */
	struct xorg_list loclist;
	xorg_list_init(&loclist);

	int count = 0, totalItems = 0;
	struct smoother_pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		totalItems++;
		if (e->scheduled_TSuS <= uS) {
			xorg_list_del(&e->list);
			xorg_list_append(&e->list, &loclist);
			count++;
		}
		/* TODO: The list is time ordered so we shoud be able to break
		 * when we find a time that's beyond out window, and save CPU time.
		 */
	}
	pthread_mutex_unlock(&ctx->listMutex);

	if (count <= 0)
		return -1; /* Nothing scheduled, bail out early. */

	/* Call the callback with any scheduled packets */
	e = NULL, next = NULL;
	xorg_list_for_each_entry_safe(e, next, &loclist, list) {
		if (ctx->outputCb) {
			ctx->outputCb(ctx->userContext, e->buf, e->lengthBytes);
			ctx->totalSizeBytes -= e->lengthBytes;

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
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)p;

	pthread_detach(ctx->threadId);
	ltnpthread_setname_np(ctx->threadId, "thread-brsmooth");

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

int smoother_pcr_alloc(void **hdl, void *userContext, smoother_pcr_output_callback cb,
	int itemsPerSecond, int itemLengthBytes, uint16_t pcrPID, int inputMuxrate_bps)
{
	struct smoother_pcr_context_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	xorg_list_init(&ctx->itemsFree);
	xorg_list_init(&ctx->itemsBusy);
	pthread_mutex_init(&ctx->listMutex, NULL);
	ctx->userContext = userContext;
	ctx->outputCb = cb;
	ctx->itemLengthBytes = itemLengthBytes;
	ctx->walltimeFirstPCRuS = 0;
	ctx->pcrFirst = -1;
	ctx->pcrLast = -1;
	ctx->pcrPID = pcrPID;
	ctx->inputMuxrate_bps = inputMuxrate_bps;

	/* TODO: We probably don't need an itemspersecond fixed value, probably,
	 * calculate the number of items based on input bitrate value and
	 * a (TODO) future latency/smoothing window.
	 */
	pthread_mutex_lock(&ctx->listMutex);
	for (int i = 0; i < itemsPerSecond; i++) {
		struct smoother_pcr_item_s *item = itemAlloc(itemLengthBytes);
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

int smoother_pcr_write(void *hdl, const unsigned char *buf, int lengthBytes, struct timeval *ts)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	pthread_mutex_lock(&ctx->listMutex);
	if (xorg_list_is_empty(&ctx->itemsFree)) {
		/* Grow the free queue */
		for (int i = 0; i < 64; i++) {
			struct smoother_pcr_item_s *item = itemAlloc(ctx->itemLengthBytes);
			if (!item)
				continue;
			xorg_list_append(&item->list, &ctx->itemsFree);
		}
	}

	struct smoother_pcr_item_s *item = xorg_list_first_entry(&ctx->itemsFree, struct smoother_pcr_item_s, list);
	if (!item) {
		pthread_mutex_unlock(&ctx->listMutex);
		return -1;
	}

	xorg_list_del(&item->list);
	pthread_mutex_unlock(&ctx->listMutex);

	if (ts) {
		item->received_TSuS = makeTimestampFromTimeval(ts);
	} else {
		item->received_TSuS = makeTimestampFromNow();
	}

	/* Grow the packet buffer if we really have to */
	if (item->maxLengthBytes < lengthBytes) {
		item->buf = realloc(item->buf, lengthBytes);
		item->maxLengthBytes = lengthBytes;
	}

	memcpy(item->buf, buf, lengthBytes);
	item->lengthBytes = lengthBytes;

	/* Find the first PCR entry in this buffer, on a specific PID */
	int ret = ltntstools_queryPCR_pid(item->buf, item->lengthBytes, &item->pcrdata, ctx->pcrPID, 1 /* pktAligned */);
	if (ret == 0) {
		/* PCR found */
		if (ctx->pcrFirst == -1) {
			ctx->pcrFirst = item->pcrdata.pcr;
			ctx->walltimeFirstPCRuS = item->received_TSuS;
		}

		/* Reset number of packets received since the last PCR. */
		/* We use this along with an estimated input bitrate to calculated a sche duled output time. */
		ctx->bitsReceivedSinceLastPCR = 0;

		ctx->pcrLast = item->pcrdata.pcr; /* Cache the last stream PCR */

		/* Figure out when this packet should be scheduled for output */
		item->scheduled_TSuS = getScheduledOutputuS(ctx, item->pcrdata.pcr);
		item->pcrComputed = 0;

	} else {
		/* No PCR found. */
		/* Calculate a schedule time for this item based on lastKnown PCR, bits since then, and a cbr rate. */
		ctx->bitsReceivedSinceLastPCR += (lengthBytes * 8);
		item->pcrdata.pcr = getPCR(ctx, 0);
		item->pcrdata.pid = ctx->pcrPID;
		item->pcrdata.offset = 0;
		item->scheduled_TSuS = getScheduledOutputuS(ctx, item->pcrdata.pcr);
		item->pcrComputed = 1; /* Note that we've computed this pcr */
	}

	pthread_mutex_lock(&ctx->listMutex);
	item->seqno = ctx->seqno++;
	ctx->totalSizeBytes += item->lengthBytes;
#if 0
	itemPrint(item);
#endif
	/* TODO: This latency is fixed a 1 second. It should be adjustable. */
	if (item->scheduled_TSuS < (5 * 1000000)) {
		/* Items that occur prior to startup, before we've
		 * received an initial PCR, have a poor scheduled pcr, so we we discard them...
		 * by throwing the item back on the free q.
		 */
		itemReset(item);
		xorg_list_append(&item->list, &ctx->itemsFree);
	} else {
		/* Queue this for scheduled output */
		xorg_list_append(&item->list, &ctx->itemsBusy);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

int64_t smoother_pcr_get_size(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;
	int64_t sizeBytes = 0;

	pthread_mutex_lock(&ctx->listMutex);
	if (ctx->totalSizeBytes > 0)
		sizeBytes = ctx->totalSizeBytes;
	pthread_mutex_unlock(&ctx->listMutex);

	return sizeBytes;
}

void smoother_pcr_reset(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	pthread_mutex_lock(&ctx->listMutex);
	ctx->walltimeFirstPCRuS = 0;
	ctx->pcrFirst = -1;
	ctx->pcrLast = -1;
	ctx->totalSizeBytes = 0;

	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_pcr_item_s *item = xorg_list_first_entry(&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		itemReset(item);
		xorg_list_del(&item->list);
		xorg_list_append(&item->list, &ctx->itemsFree);
	}

	pthread_mutex_unlock(&ctx->listMutex);
}
