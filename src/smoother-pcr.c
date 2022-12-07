/* Copyright LiveTimeNet, Inc. 2021. All Rights Reserved. */

#include <unistd.h>

#include "libltntstools/ltntstools.h"
#include "xorg-list.h"

struct smoother_pcr_item_s
{
	struct xorg_list list;
	uint64_t       seqno; /* Unique number per item, so we can check for loss/corruption in the lists. */

	unsigned char *buf;
	int            lengthBytes;
	int            maxLengthBytes;

	int            pcrComputed; /* Boolean. Was the PCR in this item computed from a base offset, or read from stream? */
	int64_t        pcrIntervalPerPacketTicks;

	struct ltntstools_pcr_position_s pcrdata; /* PCR value from pid N in the buffer, first PCR only. */
	uint64_t       received_TSuS;  /* Item received timestamp Via makeTimestampFromNow */
	uint64_t       scheduled_TSuS; /* Time this item is schedule for push via thread for smoothing output. */
};

void itemPrint(struct smoother_pcr_item_s *item)
{
	printf("seqno %" PRIu64, item->seqno);
	printf(" lengthBytes %5d", item->lengthBytes);
	printf(" received_TSuS %" PRIu64, item->received_TSuS);
	printf(" scheduled_TSuS %" PRIu64, item->scheduled_TSuS);
	printf(" pcrComputed %d", item->pcrComputed);
	printf(" pcr %" PRIi64 "\n", item->pcrdata.pcr);
}

/* byte_array.... ---------- */
struct byte_array_s
{
	uint8_t *buf;
	int maxLengthBytes;
	int lengthBytes;
};

int byte_array_init(struct byte_array_s *ba, int lengthBytes)
{
	ba->buf = malloc(lengthBytes);
	if (!ba->buf)
		return -1;

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

int byte_array_append(struct byte_array_s *ba, const uint8_t *buf, int lengthBytes)
{
	if (ba->lengthBytes + lengthBytes > ba->maxLengthBytes) {
		ba->buf = realloc(ba->buf, ba->lengthBytes + lengthBytes);
		ba->maxLengthBytes += lengthBytes;
	}
	memcpy(ba->buf + ba->lengthBytes, buf, lengthBytes);
	ba->lengthBytes += lengthBytes;

	return lengthBytes;
}

void byte_array_trim(struct byte_array_s *ba, int lengthBytes)
{
	if (lengthBytes > ba->lengthBytes)
		return;

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
	struct xorg_list itemsBusy;
	pthread_mutex_t listMutex;

	void *userContext;
	smoother_pcr_output_callback outputCb;

	uint64_t walltimeFirstPCRuS;
	int64_t pcrFirst;
	int64_t pcrLast;
	uint16_t pcrPID;

	int latencyuS;
	uint64_t bitsReceivedSinceLastPCR;

	uint64_t seqno;
	uint64_t last_seqno;

	int itemLengthBytes;
	pthread_t threadId;
	int threadRunning, threadTerminate, threadTerminated;

	int64_t totalSizeBytes;

	/* A contigious chunk of ram containing transport packets, in order.
	 * starting with a transport packet containing a PCR on pid ctx->pcrPid
	 */
	struct byte_array_s ba;
};

/* based on first received pcr, and first received walltime, compute a new walltime
 * for this new input pcr.
 */
static uint64_t getScheduledOutputuS(struct smoother_pcr_context_s *ctx, int64_t pcr)
{
	int64_t ticks = ltntstools_scr_diff(ctx->pcrFirst, pcr);
	uint64_t scheduledTimeuS = ctx->walltimeFirstPCRuS + (ticks / 27);

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

	byte_array_free(&ctx->ba);
	free(ctx);
}

/*  Service the busy list. Find any items due for output
 *  and send via the callback.
 *  It's important that we hole the mutex for a short time so we don't block
 *  the _write() method.
 */
static int _queueProcess(struct smoother_pcr_context_s *ctx, int64_t uS)
{
	/* Take any node on the Busy list up to and including items with a timestamp of uS.
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

	/* Process the local list.
	 * Call the callback with any scheduled packets
	 */
	e = NULL, next = NULL;
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

			ctx->outputCb(ctx->userContext, e->buf, e->lengthBytes, array, arrayLength);
			ctx->totalSizeBytes -= e->lengthBytes;

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
	int itemsPerSecond, int itemLengthBytes, uint16_t pcrPID, int latencyMS)
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
	ctx->latencyuS = latencyMS * 1000;
	byte_array_init(&ctx->ba, 8000 * 188); /* Initial size of 300mbps with 40ms PCR intervals */


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

int smoother_pcr_write2(void *hdl, const unsigned char *buf, int lengthBytes, int64_t pcrValue, int64_t pcrIntervalPerPacketTicks)
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

	item->received_TSuS = makeTimestampFromNow();
	item->pcrIntervalPerPacketTicks = pcrIntervalPerPacketTicks;

	/* Grow the packet buffer if we really have to */
	if (item->maxLengthBytes < lengthBytes) {
		item->buf = realloc(item->buf, lengthBytes);
		item->maxLengthBytes = lengthBytes;
	}

	memcpy(item->buf, buf, lengthBytes);
	item->lengthBytes = lengthBytes;

	/* PCR found */
	item->pcrdata.pcr = pcrValue;
	if (ctx->pcrFirst == -1) {
		ctx->pcrFirst = item->pcrdata.pcr;
		ctx->walltimeFirstPCRuS = item->received_TSuS;
	}

	/* Reset number of packets received since the last PCR. */
	/* We use this along with an estimated input bitrate to calculated a sche duled output time. */
	ctx->bitsReceivedSinceLastPCR = 0;

	ctx->pcrLast = item->pcrdata.pcr; /* Cache the last stream PCR */

	/* Figure out when this packet should be scheduled for output */
	item->scheduled_TSuS = getScheduledOutputuS(ctx, pcrValue);
	item->pcrComputed = 0;

	pthread_mutex_lock(&ctx->listMutex);
	item->seqno = ctx->seqno++;
	ctx->totalSizeBytes += item->lengthBytes;
#if 0
	itemPrint(item);
#endif
	/* Queue this for scheduled output */
	xorg_list_append(&item->list, &ctx->itemsBusy);
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

int smoother_pcr_write(void *hdl, const unsigned char *buf, int lengthBytes, struct timeval *ts)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	byte_array_append(&ctx->ba, buf, lengthBytes);

	struct ltntstools_pcr_position_s *array = NULL;
	int arrayLength = 0;
	int r = ltntstools_queryPCRs(ctx->ba.buf, ctx->ba.lengthBytes, 0, &array, &arrayLength);
	if (r < 0)
		return 0;

	struct ltntstools_pcr_position_s *pcr[2] = { 0 };

	int pcrCount = 0;
	for (int i = 0; i < arrayLength; i++) {
		struct ltntstools_pcr_position_s *e = &array[i];
		if (e->pid == ctx->pcrPID) {
			pcrCount++;
			if (pcrCount == 1 && pcr[0] == NULL)
				pcr[0] = e;
			if (pcrCount == 2 && pcr[1] == NULL)
				pcr[1] = e;
		}
		if (pcrCount == 2)
			break;
	}

	if (pcrCount < 2)
		return 0;


	int byteCount = (pcr[1]->offset - pcr[0]->offset);
	int pktCount = byteCount / 188;
	int64_t pcrIntervalPerPacketTicks = ltntstools_scr_diff(pcr[0]->pcr, pcr[1]->pcr) / pktCount;

#if 0
	/* We have two PCRs for the same PID, let's extract and calculate transport packet times. */
	printf("b.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[0]->pcr, pcr[0]->offset, pcr[0]->pid);
	printf("e.pcr = %14" PRIi64 ", %8" PRIu64 ", %04x\n", pcr[1]->pcr, pcr[1]->offset, pcr[1]->pid);
	printf("pcrIntervalPerPacketTicks = %" PRIi64 "\n", pcrIntervalPerPacketTicks);
#endif

	int64_t pcrValue = pcr[0]->pcr;

	int idx = 0;
	int rem = byteCount;
	while (rem > 0) {
		int cplen = 7 * 188;
		if (cplen > rem)
			cplen = rem;

		smoother_pcr_write2(ctx, &ctx->ba.buf[ pcr[0]->offset + idx ], cplen, pcrValue, pcrIntervalPerPacketTicks);

		pcrValue = ltntstools_scr_add(pcrValue, pcrIntervalPerPacketTicks * (cplen / 188));

		rem -= cplen;
		idx += cplen;
	}

	byte_array_trim(&ctx->ba, pcr[1]->offset);

	free(array);

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
