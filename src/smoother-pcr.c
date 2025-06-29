/* Copyright LiveTimeNet, Inc. 2021. All Rights Reserved. */

#include <unistd.h>
#include <sys/errno.h>

#include "libltntstools/ltntstools.h"
#include "xorg-list.h"

#define LOCAL_DEBUG 0

struct smoother_pcr_item_s
{
	struct xorg_list list;
	uint64_t       seqno; /* Unique number per item, so we can check for loss/corruption in the lists. */

	unsigned char *buf;
	unsigned int   lengthBytes;
	unsigned int   maxLengthBytes;

	int            pcrComputed; /* Boolean. Was the PCR in this item computed from a base offset, or read from stream? */
	int64_t        pcrIntervalPerPacketTicks; /* 27MHz */

	struct ltntstools_pcr_position_s pcrdata; /* PCR value from pid N in the buffer, first PCR only. */

	/* Under no circumstances should received_TSuS be smaller than (now - (4 * ctx->latencyuS)) */
	uint64_t       received_TSuS;  /* Item received timestamp Via makeTimestampFromNow */

	/* Under no circumstances should scheduled_TSuS be larger than (now + (4 * ctx->latencyuS)) */
	uint64_t       scheduled_TSuS; /* Time this item is schedule for push via thread for smoothing output. */

	int            pcrDidReset; /* Boolean */
};

void itemPrint(struct smoother_pcr_item_s *item)
{
static struct smoother_pcr_item_s olditem;

int64_t gap = item->scheduled_TSuS - olditem.scheduled_TSuS;

	printf("seqno %" PRIu64, item->seqno);
	printf(" lengthBytes %5d", item->lengthBytes);
	printf(" received_TSuS %" PRIu64, item->received_TSuS);
	printf(" scheduled_TSuS %" PRIu64, item->scheduled_TSuS);
	printf(" pcrComputed %d", item->pcrComputed);
	printf(" pcr %" PRIi64 "  pcrDidReset %d  schgap %" PRIi64 "\n", item->pcrdata.pcr, item->pcrDidReset, gap);

olditem = *item;
}

/* byte_array.... ---------- */
struct byte_array_s
{
	uint8_t *buf;
	unsigned int maxLengthBytes;
	unsigned int lengthBytes;
};

int byte_array_init(struct byte_array_s *ba, unsigned int lengthBytes)
{
	ba->buf = malloc(lengthBytes);
	if (!ba->buf) {
		return -1;
	}

	ba->maxLengthBytes = lengthBytes;
	ba->lengthBytes = 0;

	return 0;
}

void byte_array_free(struct byte_array_s *ba)
{
	free(ba->buf);
	ba->lengthBytes = 0;
	ba->maxLengthBytes = 0;
}

int byte_array_append(struct byte_array_s *ba, const uint8_t *buf, unsigned int lengthBytes)
{
	int newLengthBytes = ba->lengthBytes + lengthBytes;

	if (newLengthBytes > ba->maxLengthBytes) {
		/* XXX: consider exponential reallocation */
		ba->buf = realloc(ba->buf, newLengthBytes);
		ba->maxLengthBytes = newLengthBytes;
	}
	memcpy(ba->buf + ba->lengthBytes, buf, lengthBytes);
	ba->lengthBytes = newLengthBytes;

	return newLengthBytes;
}

void byte_array_trim(struct byte_array_s *ba, unsigned int lengthBytes)
{
	if (lengthBytes > ba->lengthBytes) {
		return;
	}

	memmove(ba->buf, ba->buf + lengthBytes, ba->lengthBytes - lengthBytes);
	ba->lengthBytes -= lengthBytes;
}

const uint8_t *byte_array_addr(struct byte_array_s *ba)
{
	return ba->buf;
}
/* byte_array.... ---------- */

struct smoother_pcr_context_s
{
	struct xorg_list itemsFree;
	uint64_t qFreeCount;
	struct xorg_list itemsBusy;
	uint64_t qBusyCount;
	pthread_mutex_t listMutex;
	int64_t totalUserBytes;       /**< total number of bytes held on the busy queue, for scheduled output. */
	int itemLengthBytes;          /**< Typically 7 * 188, is the allocation size for each queue item. Be skeptical when a different value. */
	pthread_cond_t item_add;      /**< signalling on queue addition */

	void *userContext;
	smoother_pcr_output_callback outputCb;

	uint64_t walltimeFirstPCRuS; /**< Reset this when the clock significantly leaps backwards */
	int64_t pcrFirst; /**< Reset this when the clock significantly leaps backwards */
	int64_t pcrTail; /**< PCR on the first list item */
	int64_t pcrHead; /**< PCR on the last list item */
	uint16_t pcrPID;

	unsigned int blockingWrites; /**< default no. No writes are blocked */
	unsigned int verbose;        /**< Dump developer/runtime stats */
	int latencyuS;
	uint64_t bitsReceivedSinceLastPCR;

	uint64_t seqno;
	uint64_t last_seqno;

	pthread_t threadId;
	int threadRunning, threadTerminate, threadTerminated;

	/* A contigious chunk of ram containing transport packets, in order.
	 * starting with a transport packet containing a PCR on pid ctx->pcrPid
	 */
	struct byte_array_s ba;

	/* Handle the case where the PCR goes forward or back in time,
	 * in our case by more than 15 seconds.
	 * Flag an internal PCR reset and let the implementation recompute its clocks.
	 */
	int didPcrReset;
	time_t lastPcrResetTime;
	int64_t pcrIntervalPerPacketTicksLast; /* 27MHz */
	int64_t pcrIntervalTicksLast; /* 27MHz */

	int64_t measuredLatencyMs; /* based on first and last PCRs in the list, how much latency do we have? */
	int64_t measuredLatencyMs_hwm; /* based on first and last PCRs in the list, how much latency do we have - maximum */

	struct ltn_histogram_s *histReceive;
	struct ltn_histogram_s *histTransmit;

	/* Sum total of all items and their realloc() / alloc sizes.
	 */
	uint64_t totalAllocFootprintBytes;

	/* Number of items the free list was empty, and we grew it by 1 item. none zero values here
	 * suggests the entire context was improperly sized by the caller during smoother_pcr_alloc()
	 */
	uint64_t totalItemGrowth;
	uint64_t totalItems;
};

int smoother_pcr_get_statistics(void *hdl, struct smoother_pcr_statistics *s)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;
	if (!ctx || !s) {
		return -1;
	}

	s->measuredLatencyMs_hwm = ctx->measuredLatencyMs_hwm;
	s->totalAllocFootprintBytes = ctx->totalAllocFootprintBytes;
	s->totalItemGrowth = ctx->totalItemGrowth;
	s->totalItems = ctx->totalItems;
	s->totalUserBytes = ctx->totalUserBytes;
	s->qBusyCount = ctx->qBusyCount;
	s->qFreeCount = ctx->qFreeCount;

	return 0; /* Success */
}

/* based on first received pcr, and first received walltime, compute a new walltime
 * for this new input pcr.
 */
static uint64_t getScheduledOutputuS(struct smoother_pcr_context_s *ctx, int64_t pcr, int64_t pcrIntervalTicks)
{
	int64_t ticks = ltntstools_scr_diff(ctx->pcrFirst, pcr);

#if 0
// ST - Disabled. This patch breaks the smoothing in the mux.
// The mux as of 12/3/24 outputs PCRs at intervals of 30ms and occasionally 1ms.
// This variance in the pcr interval causes the smoother to improperly schedule
// out packets, in order to reduce the amount of 'data buffered' in th smoother itself.
// The design assumes we always buffer a moderatly fixed interval (PCR) of data
// and then we compensate for the (mostly static) interval when packet scheduling.
// A wildly varying interval results in the packets being improperly scheduled with
// bursts and stalls as the pcr interval is constantly re-compputed and the buffer
// contains too little or too much.
	/* Reduce by one PCR interval due to the buffering in pcr_smoother_write */
	ticks -= pcrIntervalTicks;
#endif

	uint64_t scheduledTimeuS = ctx->walltimeFirstPCRuS + (ticks / 27);

	/* Add user defined latency */
	scheduledTimeuS += ctx->latencyuS;

	return scheduledTimeuS;
}

static inline uint64_t makeTimestampFromTimeval(struct timeval *ts)
{
	uint64_t t = ((int64_t)ts->tv_sec * 1000000LL) + ts->tv_usec;
	return t;
}

static inline uint64_t makeTimestampFromNow()
{
	struct timeval now;
	gettimeofday(&now, NULL);
	return makeTimestampFromTimeval(&now);
}

static inline uint64_t makeTimestampFrom1SecondAgo()
{
	struct timeval now;
	gettimeofday(&now, NULL);
	now.tv_sec--;
	return makeTimestampFromTimeval(&now);
}

static inline uint64_t makeTimestampFrom2SecondAgo()
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
	if (!item) {
		return NULL;
	}

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
static void _queuePrintList(struct smoother_pcr_context_s *ctx, struct xorg_list *head, const char *name)
{
	int totalItems = 0;

	printf("Queue %s -->\n", name);
	struct smoother_pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, head, list) {
		totalItems++;
		itemPrint(e);
	}
	printf("Queue End --> %d items\n", totalItems);
}
#endif

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
		ctx->qFreeCount--;
		itemFree(item);
	}
	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_pcr_item_s *item = xorg_list_first_entry(&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		xorg_list_del(&item->list);
		ctx->qBusyCount--;
		itemFree(item);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	byte_array_free(&ctx->ba);

	ltn_histogram_free(ctx->histReceive);
	ltn_histogram_free(ctx->histTransmit);

	free(ctx);
}

/* Take any node on the Busy list up to and including items with a timestamp of uS.
 * Put them on a local list so we can free the holding mutex as fast as possible.
 * We're being called with a thread holding the listmutex, don't linger.
 * 
 * The list is expected to have items with e->scheduled_TSuS <= uS
 * and items where e->scheduled_TSuS > uS
 */
static int _queueProcess_makeloc(struct smoother_pcr_context_s *ctx, int64_t uS, struct xorg_list *loclist)
{
	int count = 0, totalItems = 0, redundantItems = 0;
	struct smoother_pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		totalItems++;

		if (totalItems == 1) {
			ctx->pcrHead = e->pcrdata.pcr;
		}

		if (e->scheduled_TSuS <= uS) {
			xorg_list_del(&e->list);
			ctx->qBusyCount--;
			xorg_list_append(&e->list, loclist);
			count++;
		} else {
			if (count > 0) {
				/* Time ordering problem on the list now. Dump both lists and abort, later.*/
				redundantItems++;
			}
			break; /* Everything beyond this point is in the future, don't service it yet. */
		}
	}
	return count;
}

#if LOCAL_DEBUG
static void _queueProcess_checkbusy(struct smoother_pcr_context_s *ctx, int64_t uS, struct xorg_list *loclist)
{
	/* Make sure the busy list is contigious */
	int countSeq = 0;
	uint64_t last_seq = 0;
	struct smoother_pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		countSeq++;

		if (countSeq > 1) {
			if (last_seq + 1 != e->seqno) {
				/* Almost certainly, the schedule US time is out of order, warn. */
				printf("List possibly mangled, seqnos might be bad now, %" PRIu64 ", %" PRIu64 "\n", last_seq, e->seqno);
#if LOCAL_DEBUG
				_queuePrintList(ctx, &ctx->itemsBusy, "Busy");
				_queuePrintList(ctx, loclist, "loclist");
				fflush(stdout);
				fflush(stderr);
				exit(1);
#endif
			}
		}
		last_seq = e->seqno;
	}
}
#endif

/*  Service the busy list. Find any items due for output
 *  and send via the callback.
 *  It's important that we hold the mutex for a short time so we don't block
 *  the _write() method.
 *  Returns: 0 when something was processed, < 0 when nothing was to be done.
 */
static int _queueProcess(struct smoother_pcr_context_s *ctx, int64_t uS)
{
	/* Take any node on the Busy list up to and including items with a timestamp of uS.
	 * Put them on a local list so we can free the holding mutex as fast as possible
	 */
	struct xorg_list loclist;
	xorg_list_init(&loclist);

	int count = _queueProcess_makeloc(ctx, uS, &loclist);

#if LOCAL_DEBUG
	_queueProcess_checkbusy(ctx, uS, &loclist); /* The performance of this sucks */
#endif

	pthread_mutex_unlock(&ctx->listMutex);

	if (count <= 0) {
		return -1; /* Nothing scheduled, bail out early. */
	}

	/* Process the local list.
	 * Call the callback with any scheduled packets
	 */
	struct smoother_pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &loclist, list) {
		if (ctx->outputCb) {

			/* Create a PCR value for EVERY packet in the buffer,
			 * let the callee decide what to do with them.
			 */
			struct ltntstools_pcr_position_s *array = NULL;
			int arrayLength = 0;
			for (int i = 0; i < e->lengthBytes / 188; i++) {
				struct ltntstools_pcr_position_s p;
				p.offset = i * 188;
				p.pcr = e->pcrdata.pcr + (i * e->pcrIntervalPerPacketTicks);
				p.pid = ltntstools_pid(e->buf + (i * 188));
				ltntstools_pcr_position_append(&array, &arrayLength, &p);
			}

			struct timeval tv;
			gettimeofday(&tv, NULL);
			ltn_histogram_interval_update(ctx->histTransmit, &tv);

			int x = e->lengthBytes;
			uint64_t sn = e->seqno;

			ctx->outputCb(ctx->userContext, e->buf, e->lengthBytes, array, arrayLength);
			if (x != e->lengthBytes) {
				printf("%s() ERROR %d != %d, mangled returned object length\n", __func__, x, e->lengthBytes);
			}
			if (sn != e->seqno) {
				printf("%s() ERROR %" PRIu64 " != %" PRIu64 ", mangled returned object seqno\n", __func__, sn, e->seqno);
			}

			pthread_mutex_lock(&ctx->listMutex);			
			ctx->totalUserBytes -= e->lengthBytes;
			pthread_mutex_unlock(&ctx->listMutex);

			free(array);

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
		ctx->qFreeCount++;
	}
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

extern int ltnpthread_setname_np(pthread_t thread, const char *name);

static void * smoother_pcr_threadFunc(void *p)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)p;

	pthread_detach(ctx->threadId);
	ltnpthread_setname_np(ctx->threadId, "thread-brsmooth");

	ctx->threadTerminated = 0;
	ctx->threadRunning = 1;

	int tocount = 0, rocount = 0, okcount = 0;
	int q1count = 0, q2count = 0;
	int ret;

	struct timespec abstime = { 0, 50 * 1000 };

	while (!ctx->threadTerminate) {

		pthread_mutex_lock(&ctx->listMutex);

		if (xorg_list_is_empty(&ctx->itemsBusy)) {
			ret = pthread_cond_timedwait(&ctx->item_add, &ctx->listMutex, &abstime);
		} else {
			ret = 0;
		}

		if (ret == ETIMEDOUT) {
			pthread_mutex_unlock(&ctx->listMutex);
			tocount++;
			continue;
		} else
		if (ret == 0) {
			okcount++;

			int64_t uS = makeTimestampFromNow();

			/* Service the output schedule queue, output any UDP packets when they're due.
			 * Important to remember that we're calling this func while we're holding the mutex.
			 * The function eventually unlocks the mutex.
			 */
			if (_queueProcess(ctx, uS) < 0) {
				q1count++;
				usleep(1 * 1000);
			} else {
				q2count++;
			}
		} else {
			rocount++;
			pthread_mutex_unlock(&ctx->listMutex);
		}
	}
#if LOCAL_DEBUG
	/* Show code path counts */
	printf("to %d ro %d ok %d\n", tocount, rocount, okcount);
	printf("q1 %d q2 %d\n", q1count, q2count);
#endif
	ctx->threadRunning = 1;
	ctx->threadTerminated = 1;

	/* TODO: pthread detach else we'll cause a small leak in valgrind. */
	return NULL;
}

int smoother_pcr_alloc(void **hdl, void *userContext, smoother_pcr_output_callback cb,
	int itemsPerSecond, int itemLengthBytes, uint16_t pcrPID, int latencyMS)
{
	struct smoother_pcr_context_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx || pcrPID <= 16 || pcrPID > 0x1ffe || latencyMS < 50 || itemLengthBytes != (7*188)) {
		return -1;
	}

	pthread_cond_init(&ctx->item_add, NULL);
	xorg_list_init(&ctx->itemsFree);
	xorg_list_init(&ctx->itemsBusy);
	pthread_mutex_init(&ctx->listMutex, NULL);
	ctx->userContext = userContext;
	ctx->outputCb = cb;
	ctx->itemLengthBytes = itemLengthBytes;
	ctx->walltimeFirstPCRuS = 0;
	ctx->pcrFirst = -1;
	ctx->pcrTail = -1;
	ctx->pcrHead = -1;
	ctx->pcrPID = pcrPID;
	ctx->latencyuS = latencyMS * 1000;
	ctx->lastPcrResetTime = time(NULL);
	ctx->blockingWrites = 0;
	byte_array_init(&ctx->ba, 8000 * 188); /* Initial size of 300mbps with 40ms PCR intervals */

	ltn_histogram_alloc_video_defaults(&ctx->histReceive, "receive arrival times");
	ltn_histogram_alloc_video_defaults(&ctx->histTransmit, "transmit arrival times");

	/* TODO: We probably don't need an itemspersecond fixed value, probably,
	 * calculate the number of items based on input bitrate value and
	 * a (TODO) future latency/smoothing window.
	 */
	pthread_mutex_lock(&ctx->listMutex);
	for (int i = 0; i < itemsPerSecond; i++) {
		struct smoother_pcr_item_s *item = itemAlloc(itemLengthBytes);
		if (!item) {
			continue;
		}
		xorg_list_append(&item->list, &ctx->itemsFree);
		ctx->qFreeCount++;
		ctx->totalItems++;
		ctx->totalAllocFootprintBytes += itemLengthBytes;
	}
	pthread_mutex_unlock(&ctx->listMutex);

	/* Spawn a thread that manages the scheduled output queue. */
	pthread_create(&ctx->threadId, NULL, smoother_pcr_threadFunc, ctx);

	*hdl = ctx;

	return 0;
}

/* Ideally:
 * cplen: is 7 * 188, or a multiple of 188 less. Never greater than 7 * 188.
 * pcrIntervalPerPacketTicks: Number of ticks (27MHz) a single TS packet is worth
 * pcrIntervalTicks: Number of ticks (27MHz) between the last two PCRs detected.
 */
static int smoother_pcr_write2(void *hdl, const unsigned char *buf, unsigned int lengthBytes, int64_t pcrValue,
	int64_t pcrIntervalPerPacketTicks, int64_t pcrIntervalTicks)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	if (ctx->blockingWrites) {
		/* Blocking writes - Prevent faster than realitime filling of our queues, which will extend
		 * indefintely until all platform memory is consumed (worst cast).
		 */
		while (1) {
			pthread_mutex_lock(&ctx->listMutex);
			if (!xorg_list_is_empty(&ctx->itemsFree)) {
				break;
			}
			pthread_mutex_unlock(&ctx->listMutex);

			if (ctx->verbose) {
				char ts[256];
				time_t now = time(0);
				sprintf(ts, "%s", ctime(&now));
				ts[ strlen(ts) - 1] = 0;

				struct smoother_pcr_statistics s;
				smoother_pcr_get_statistics(ctx, &s);

				printf("%s: Dev Statistics - max observed latency %6" PRIi64 "(ms) alloc %8" PRIi64  
					"(B) used %8" PRIu64 "(B) items %5" PRIu64" free %5" PRIu64 " busy %5" PRIu64 " growth %5" PRIu64 " \n",
					ts,
					s.measuredLatencyMs_hwm,
					s.totalAllocFootprintBytes,
					s.totalUserBytes,
					s.totalItems,
					s.qFreeCount,
					s.qBusyCount,
					s.totalItemGrowth);
			}

			usleep(100 * 1000);
		}
	} else {
		/* None-blocking */
		/* Consume as much platform ram to hold faster than realtime writes (from S3). */
		pthread_mutex_lock(&ctx->listMutex);
		if (xorg_list_is_empty(&ctx->itemsFree)) {
			/* Grow the free queue */
			for (int i = 0; i < 64; i++) {
				struct smoother_pcr_item_s *item = itemAlloc(ctx->itemLengthBytes);
				if (!item) {
					continue;
				}
				xorg_list_append(&item->list, &ctx->itemsFree);
				ctx->qFreeCount++;
				ctx->totalItemGrowth++;
				ctx->totalItems++;
			}
		}
	}

	struct smoother_pcr_item_s *item = xorg_list_first_entry(&ctx->itemsFree, struct smoother_pcr_item_s, list);
	if (!item) {
		pthread_mutex_unlock(&ctx->listMutex);
		return -1;
	}

	xorg_list_del(&item->list);
	ctx->qFreeCount--;
	pthread_mutex_unlock(&ctx->listMutex);

	item->received_TSuS = makeTimestampFromNow();
	item->pcrIntervalPerPacketTicks = pcrIntervalPerPacketTicks;

	/* Grow the packet buffer if we really have to */
	if (item->maxLengthBytes < lengthBytes) {
		item->buf = realloc(item->buf, lengthBytes);
		item->maxLengthBytes = lengthBytes;
		ctx->totalAllocFootprintBytes += lengthBytes;
	}

	memcpy(item->buf, buf, lengthBytes);
	item->lengthBytes = lengthBytes;

	/* PCR found */
	item->pcrdata.pcr = pcrValue;
	if (ctx->pcrFirst == -1) {
#if LOCAL_DEBUG
		printf("ctx->pcrFirst    was    %" PRIi64 ", ctx->walltimeFirstPCRuS %" PRIi64 "\n",
			ctx->pcrFirst, ctx->walltimeFirstPCRuS);
#endif
		ctx->pcrFirst = item->pcrdata.pcr;
		ctx->walltimeFirstPCRuS = item->received_TSuS;

#if LOCAL_DEBUG
		printf("ctx->pcrFirst reset to %" PRIi64 ", ctx->walltimeFirstPCRuS %" PRIi64 "\n",
			ctx->pcrFirst, ctx->walltimeFirstPCRuS);
#endif
	}

	/* Reset number of packets received since the last PCR. */
	/* We use this along with an estimated input bitrate to calculated a sche duled output time. */
	ctx->bitsReceivedSinceLastPCR = 0;

	ctx->pcrTail = item->pcrdata.pcr; /* Cache the last stream PCR */

	/* Figure out when this packet should be scheduled for output */
	item->scheduled_TSuS = getScheduledOutputuS(ctx, pcrValue, pcrIntervalTicks);
	item->pcrComputed = 0;

	pthread_mutex_lock(&ctx->listMutex);
	item->seqno = ctx->seqno++;
	ctx->totalUserBytes += item->lengthBytes;
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
		struct smoother_pcr_item_s *last = xorg_list_last_entry(&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		//struct smoother_pcr_item_s *last = ctx->itemsBusy.prev;
		if (last->scheduled_TSuS > item->scheduled_TSuS) {
			/* Previous versions of this design added 1 tick to the scheduled_TSuS.
			 * This works well when the input jitter of the stream is fairly low, such
			 * as from a udp network, with IATs in the 10's of ms range.
			 * It doesn't work very well when input jitter is seconds, such as when
			 * a S3 to smoother project is being developed.
			 * The right answer seems to be, calculate the length of the item we're planning
			 * to transmit in packetTicks, and convert to uS.
			 */
			int64_t t_uS = (ctx->pcrIntervalPerPacketTicksLast * (item->lengthBytes / 188)) / 27;
			item->scheduled_TSuS = last->scheduled_TSuS + t_uS;
		}
	}

	/* Queue this for scheduled output */
	xorg_list_append(&item->list, &ctx->itemsBusy);
	ctx->qBusyCount++;

	pthread_cond_signal(&ctx->item_add);
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

/* Main entry point for packets into the smoother.
 * We'll call other helper functions eg _write2 to assist with the process and simplify
 * the readability.
 */
int smoother_pcr_write(void *hdl, const unsigned char *buf, int lengthBytes, struct timeval *ts)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	ltn_histogram_interval_update(ctx->histReceive, ts);
#if LOCAL_DEBUG
	ltn_histogram_interval_print(STDOUT_FILENO, ctx->histReceive, 5);
	ltn_histogram_interval_print(STDOUT_FILENO, ctx->histTransmit, 5);
#endif

	/* append all payload into a large buffer */
	byte_array_append(&ctx->ba, buf, lengthBytes);

	int pcrCount;

	do {
		/* Search this buffer for any PCRs */
		struct ltntstools_pcr_position_s *array = NULL;
		int arrayLength = 0;
		int r = ltntstools_queryPCRs(ctx->ba.buf, ctx->ba.lengthBytes, 0, &array, &arrayLength);
		if (r < 0) {
			return 0;
		}

		/* Find the first two PCRs for the user preferred PID, skip any other pids/pcrs */
		struct ltntstools_pcr_position_s *pcr[2] = { 0 };
		pcrCount = 0;

		for (int i = 0; i < arrayLength; i++) {
			struct ltntstools_pcr_position_s *e = &array[i];
			if (e->pid == ctx->pcrPID) {
				pcrCount++;
				if (pcrCount == 1 && pcr[0] == NULL)
					pcr[0] = e;
				if (pcrCount == 2 && pcr[1] == NULL)
					pcr[1] = e;
			}
			/* Count up to a third PCR, in case we need to handle multiple intervals */
			if (pcrCount == 3) {
				break;
			}
		}

		/* We need atleast two PCRs for interval and timing calculations */
		if (pcrCount < 2) {
			/* Bail out, we'll try again later when more packets are available */
			free(array);
			return 0;
		}

		/* Amount of payload between the first two consecutive PCRs */
		int byteCount = (pcr[1]->offset - pcr[0]->offset);

		int pktCount = byteCount / 188;

		/* 27MHz clock units */
		int64_t pcrIntervalTicks = ltntstools_scr_diff(pcr[0]->pcr, pcr[1]->pcr);
		int64_t pcrIntervalPerPacketTicks = pcrIntervalTicks / pktCount;

		if (pcrIntervalTicks > (15 * 27000000)) {
			printf("Detected significant pcr jump:\n");
			if (pcr[0]->pcr < pcr[1]->pcr) {
				printf("  - forwards\n");
				printf("  - b.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[0]->pcr, pcr[0]->offset, pcr[0]->pid);
				printf("  - e.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[1]->pcr, pcr[1]->offset, pcr[1]->pid);
			}
			if (pcr[0]->pcr > pcr[1]->pcr) {
				printf("  - backwards\n");
				printf("  - b.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[0]->pcr, pcr[0]->offset, pcr[0]->pid);
				printf("  - e.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[1]->pcr, pcr[1]->offset, pcr[1]->pid);
			}

	//		_queuePrintList(ctx, &ctx->itemsBusy, "Busy");

			ctx->didPcrReset = 1;

			/* Fixup the pcrIntervalPerPacketTicks and preset the reset the internal walltime and other pcr clocks */
			printf("Auto-correcting PCR schedule due to PCR timewrap. "
				"pcrIntervalPerPacketTicks %" PRIi64 " to %" PRIi64 ", "
				"pcrIntervalTicks %" PRIi64 " to %" PRIi64 "\n",
				pcrIntervalPerPacketTicks, ctx->pcrIntervalPerPacketTicksLast,
				pcrIntervalTicks, ctx->pcrIntervalTicksLast);
			pcrIntervalPerPacketTicks = ctx->pcrIntervalPerPacketTicksLast;
			pcrIntervalTicks = ctx->pcrIntervalTicksLast;
		}

		ctx->measuredLatencyMs = ltntstools_scr_diff(ctx->pcrHead, ctx->pcrTail) / 27000;
		ctx->measuredLatencyMs_hwm = (ctx->measuredLatencyMs > ctx->measuredLatencyMs_hwm) ? ctx->measuredLatencyMs : ctx->measuredLatencyMs_hwm;
#if LOCAL_DEBUG
		{
			/* Dump the first and second PCR we found. */
			printf("b.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[0]->pcr, pcr[0]->offset, pcr[0]->pid);
			printf("e.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[1]->pcr, pcr[1]->offset, pcr[1]->pid);
			//printf("pcrHead %" PRIi64 " pcrTail %" PRIi64 "\n", ctx->pcrHead, ctx->pcrTail);
			printf("pcrIntervalPerPacketTicks = %" PRIi64 ", pktCount %d byteCount %d pcrDidReset %d totalUserBytes %" PRIi64 ", latency %" PRIi64 " ms\n",
				pcrIntervalPerPacketTicks, pktCount, byteCount, ctx->didPcrReset, ctx->totalUserBytes,
				ctx->measuredLatencyMs);
		}
#endif

		/* maintain a count based on the first PCR */
		int64_t pcrValue = pcr[0]->pcr;

		int idx = 0;
		int rem = byteCount;
		while (rem > 0) {
			int cplen = 7 * 188;
			if (cplen > rem) {
				cplen = rem;
			}

			smoother_pcr_write2(ctx, &ctx->ba.buf[ pcr[0]->offset + idx ], cplen, pcrValue,
				pcrIntervalPerPacketTicks, pcrIntervalTicks);

			/* Update the PCR based on the number of packets we're writing into the smoother, adjusting
			 * PCR by the correct number of ticks per transport packet.
			 */
			pcrValue = ltntstools_scr_add(pcrValue, pcrIntervalPerPacketTicks * (cplen / 188));

			rem -= cplen;
			idx += cplen;
		}

		byte_array_trim(&ctx->ba, pcr[1]->offset);

		free(array);
		array = NULL;

		/* If its been more than 60 seconds, reset the PCR to avoid slow drift over time.
		 * Also, prevents issues where the pcrFirst value wraps and tick calculations that
		 * drive scheduled packet output time goes back in time.
		 */
		time_t now = time(NULL);
		if (now >= ctx->lastPcrResetTime + 10) {
			ctx->lastPcrResetTime = now;
#if LOCAL_DEBUG
			printf("Triggering PCR timebase recalculation\n");
#endif
			ctx->didPcrReset = 1;
		}

		/* If the PCR resets, we need to track some pcr interval state so we can properly
		 * schedule out the remaining packets from a good PCR, without bursting, prior
		 * to restabalishing the timebase for the new PCR, we'll need this value
		 * to schedule, preserve it.
		 */
		ctx->pcrIntervalPerPacketTicksLast = pcrIntervalPerPacketTicks;
		ctx->pcrIntervalTicksLast = pcrIntervalTicks;

		/* And finally, if during this pass we detected a PCR reset, ensure the next
		 * round of writes resets its internal clocks and goes through a full PCR reset.
		 */
		if (ctx->didPcrReset) {
			ctx->pcrFirst = -1;
			ctx->didPcrReset = 0;
		}

	/* We only wrote out until just before the second PCR, so loop if we found more than
	 * two PCRs in our buffer to handle the next PCR->PCR interval as well.
	 */
	} while (pcrCount > 2);

	return 0;
}

int64_t smoother_pcr_get_size(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;
	int64_t sizeBytes = 0;

	pthread_mutex_lock(&ctx->listMutex);
	if (ctx->totalUserBytes > 0) {
		sizeBytes = ctx->totalUserBytes;
	}
	pthread_mutex_unlock(&ctx->listMutex);

	return sizeBytes;
}

int smoother_pcr_set_blocking_writes(void *hdl, unsigned int tf)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;
	if (!ctx) {
		return -1;
	}
	ctx->blockingWrites = tf;

	return 0; /* Success */
}

int smoother_pcr_set_verbose(void *hdl, unsigned int verbose)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;
	if (!ctx) {
		return -1;
	}
	ctx->verbose = verbose;

	return 0; /* Success */
}

void smoother_pcr_reset(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	pthread_mutex_lock(&ctx->listMutex);
	ctx->walltimeFirstPCRuS = 0;
	ctx->pcrFirst = -1;
	ctx->pcrHead = -1;
	ctx->pcrTail = -1;
	ctx->totalUserBytes = 0;

	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_pcr_item_s *item = xorg_list_first_entry(&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		itemReset(item);
		xorg_list_del(&item->list);
		ctx->qBusyCount--;

		xorg_list_append(&item->list, &ctx->itemsFree);
		ctx->qFreeCount++;
	}

	pthread_mutex_unlock(&ctx->listMutex);
}
