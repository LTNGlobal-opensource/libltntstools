#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libltntstools/ltntstools.h>

#define LOCAL_DEBUG 0
#define MODULE_PREFIX "vbv: "
#define DEFAULT_VBV_SIZE (800 * 1024)

extern int usleep(useconds_t usec);
extern int ltnpthread_setname_np(pthread_t thread, const char *name);
static void * vbv_threadFunc(void *p);
static int isValidFramerate(double framerate);

struct pkt_item_s
{
	struct xorg_list list;
	struct ltn_pes_packet_s pkt;
	struct timespec ts;
};

struct vbv_statistic_s
{
	struct timespec ts;
	uint64_t count;
};

struct vbv_statistics_s
{
	struct vbv_statistic_s overflow;   /**< Overflow condition and time */
	struct vbv_statistic_s underflow;  /**< Underflow condition and time */
	struct vbv_statistic_s pkt;        /**< last successful pkt addition (non-underflow) and time */
	struct vbv_statistic_s ooo_dts;    /**< DTS arrived out-of-order */
	struct vbv_statistic_s fullness;   /**< Buffer should be within 5% to 90% fullness */
};

struct vbv_ctx_s
{
	uint16_t pid;
	int verbose;

	vbv_callback cb;
	void *userContext;

	struct vbv_decoder_profile_s decoder_profile;

	/* The caller is expected to hand us pkts in DTS assending order.
	 * The list is therefore ordered in ascending order by DTS implicitly.
	 */
	struct xorg_list pktList;
	pthread_mutex_t pktListMutex; 
	uint32_t usedBytes;
	int64_t dts_lwm, dts_hwm;
	int64_t dts_last;

	struct vbv_statistics_s stats;
	int64_t decoder_stc; /**< continiously incrementing value, goes beyond 33bits */
	int64_t encoder_stc; /**< continiously incrementing value, goes beyond 33bits */

	pthread_t threadId;
	int threadRunning, threadTerminate, threadTerminated;
};

static struct vbv_bitrate_s {
	uint32_t codec;    /**< 1 = H.264 */
	uint32_t levelX10; /**< 3.1 = 31, 4.0 = 40, etc */
	uint32_t bitdepth; /**< Size of VBV buffer in bits */
} vbv_bitrates[] =
{
	{ 1,  10,     64 },
	{ 1,  11,    192 },
	{ 1,  12,    384 },
	{ 1,  13,    768 },
	{ 1,  20,   2400 },
	{ 1,  21,   4000 },
	{ 1,  22,   4000 },
	{ 1,  30,  10000 },
	{ 1,  31,  14000 },
	{ 1,  32,  20000 },
	{ 1,  40,  20000 },
	{ 1,  41,  50000 },
	{ 1,  42,  50000 },
	{ 1,  50, 135000 },
	{ 1,  51, 240000 },
};

int ltntstools_vbv_bitrate_lookup(int codec, int levelX10)
{
	for (int i = 0; i < (sizeof(vbv_bitrates) / sizeof(struct vbv_bitrate_s)); i++) {
		if (vbv_bitrates[i].codec == codec && vbv_bitrates[i].levelX10 == levelX10) {
			return (vbv_bitrates[i].bitdepth * 1000) / 8;
		}
	}

	return -1;
}

int ltntstools_vbv_profile_defaults(struct vbv_decoder_profile_s *dp, int codec, int levelX10, double framerate)
{
	if (!dp) {
		return -1;
	}

	int b = ltntstools_vbv_bitrate_lookup(codec, levelX10);
	if (b < 0) {
		return -1;
	}

	if (isValidFramerate(framerate) == 0) {
		return -1;
	}

	dp->vbv_buffer_size = b;
	dp->framerate = framerate;

	printf(MODULE_PREFIX "vbv_buf_size %d, framerate %6.2f\n", dp->vbv_buffer_size, dp->framerate);

	return 0; /* Success */
}

const char *ltntstools_vbv_event_name(enum ltntstools_vbv_event_e e)
{
	switch(e) {
	case EVENT_VBV_UNDEFINED:     return "EVENT_VBV_UNDEFINED";
	case EVENT_VBV_UNDERFLOW:     return "EVENT_VBV_UNDERFLOW";
	case EVENT_VBV_OVERFLOW:      return "EVENT_VBV_OVERFLOW";
	case EVENT_VBV_FULLNESS_PCT:  return "EVENT_VBV_FULLNESS_PCT";
	case EVENT_VBV_BPS:           return "EVENT_VBV_BPS";
	case EVENT_VBV_OOO_DTS:       return "EVENT_VBV_OOO_DTS";
	default:                      return "EVENT_VBV_UNKNOWN";
	}
}

static void statsReset(struct vbv_ctx_s *ctx)
{
	memset(&ctx->stats, 0, sizeof(ctx->stats));
}

static struct vbv_statistic_s *getStatisticForEvent(struct vbv_ctx_s *ctx, enum ltntstools_vbv_event_e e)
{
	if (e == EVENT_VBV_UNDERFLOW) {
		return &ctx->stats.underflow;
	} else
	if (e == EVENT_VBV_OVERFLOW) {
		return &ctx->stats.overflow;
	} else
	if (e == EVENT_VBV_FULLNESS_PCT) {
		return &ctx->stats.fullness;
	} else {
		printf("%s() no stat for event %s (%d), fixme\n",
			__func__, ltntstools_vbv_event_name(e), e);
	}

	return NULL;
}

static void raiseEvent(struct vbv_ctx_s *ctx, enum ltntstools_vbv_event_e e)
{
	struct vbv_statistic_s *s = getStatisticForEvent(ctx, e);
	if (s) {
		s->count++;
		clock_gettime(CLOCK_REALTIME, &s->ts);
	}

	if (ctx->verbose) {
		struct timeval ts;
		gettimeofday(&ts, NULL);
		printf("%d.%06d: %s pid 0x%04x\n", (int)ts.tv_sec, (int)ts.tv_usec, ltntstools_vbv_event_name(e), ctx->pid);
	}

	if (ctx->cb) {
		ctx->cb(ctx->userContext, e);
	}
}

/* Allocates memory from the VBV to store the PES and payload, if the action
 * of doing so would not overflow the VBV.
 * Trigger any notifications efficienly.
 */
static int addItem(struct vbv_ctx_s *ctx, const struct ltn_pes_packet_s *pkt)
{
	/* Optimistic allocation */
	struct pkt_item_s *i = calloc(1, sizeof(*i));
	if (!i) {
		return -1;
	}
	/* Shallow copy (no pointers or data) of the original pkt. Retain the
	 * payload sizes though. */
	i->pkt = *pkt;
	i->pkt.data = NULL;
	i->pkt.rawBuffer = NULL;
	clock_gettime(CLOCK_REALTIME, &i->ts);

	int overflow = 1;
	pthread_mutex_lock(&ctx->pktListMutex);
	if (ctx->usedBytes + pkt->rawBufferLengthBytes < ctx->decoder_profile.vbv_buffer_size) {

		xorg_list_append(&i->list, &ctx->pktList);
		ctx->usedBytes += i->pkt.rawBufferLengthBytes;

		/* Assume no clock, then acquire either PTS or DTS */
		int64_t clk = 0;
		if ((pkt->PTS_DTS_flags & 0x03) == 1) {
			/* PTS Only */
			clk = pkt->PTS;
		} else
		if ((pkt->PTS_DTS_flags & 0x03) == 3) {
			/* PTS and DTS */
			clk = pkt->DTS;
		}

		if (clk == ctx->dts_last) {
			/* Repeated DTS - We're OK with this */
			ctx->encoder_stc += (0 * 300);
		} else
		if (clk > ctx->dts_last) {
			/* The DTS moved forward */
			if (clk - ctx->dts_last >= 9000) {
				/* The DTS jumped forward by more then 1/10 sec.
				 * That's considered unusual.
				 */
			}
			ctx->encoder_stc += ((clk - ctx->dts_last) * 300);
		} else {
			/* DTS moved backwards - Potential clock rolled backwards */
			if (ctx->dts_last - clk >= 90000) {
				/* Clock jumped backwards by more than a second... we're going to
				 * assume the DTS clock has wrapped.
				 */
			} else
			if (ctx->dts_last - clk >= 9000) {
				/* Clock jumped backwards by more than a 1/10 sec.
				 * Highly unusual.
				 */
			} else {
				/* What now? Nothing. We allow a small amount of DTS jitter, because
				 * Either a realy decoder can deal with it or not, but how the DTS
				 * is dequeued from the VBV is irerelevant.
				 */
			}
			ctx->encoder_stc += ((MAX_PTS_VALUE - ctx->dts_last) * 300);
			ctx->encoder_stc += (clk * 300);
		}
		ctx->dts_last = clk;

		ctx->dts_hwm = clk > ctx->dts_hwm ? clk : ctx->dts_hwm;
		ctx->dts_lwm = clk != 0 && clk <= ctx->dts_lwm && clk ? clk : ctx->dts_lwm;

		overflow = 0;
	}
	pthread_mutex_unlock(&ctx->pktListMutex);

	if (overflow) {
		raiseEvent(ctx, EVENT_VBV_OVERFLOW);
		free(i);
		return -1;
	}

	return 0; /* Success */
};

uint32_t framerateToNs(double framerate)
{
	double n = 1e9 / framerate;
	return n;
}

uint32_t framerateToUs(double framerate)
{
	double n = 1e6 / framerate;
	return n * 1000;
}

uint32_t framerateToTicks(double framerate)
{
	double n = 1e6 / framerate;
	return n * 27;
}

static int isValidFramerate(double framerate)
{
	double framerates[] = { 23.98, 24, 25, 29.97, 30, 50, 59.94, 60 };

	int found = 0;
	for (int i = 0; i < (sizeof(framerates) / sizeof(double)); i++) {
		if (framerate == framerates[i]) {
			found = 1;
			break;
		}
	}
	if (!found) {
		return 0; /* Invalid framerate */
	}

	return 1; /* Profile is considered valid */
}

int ltntstools_vbv_profile_validate(struct vbv_decoder_profile_s *dp)
{
	if (isValidFramerate(dp->framerate) == 0) {
		return 0; /* No */
	}

	return 1; /* Profile is considered valid */
}

static int timesec_diff(struct timespec next_time, struct timespec last_time)
{
	struct timespec diff;
	diff.tv_sec = next_time.tv_sec - last_time.tv_sec;
	diff.tv_nsec = next_time.tv_nsec - last_time.tv_nsec;
	if (diff.tv_nsec < 0) {
		diff.tv_sec -= 1;
		diff.tv_nsec += 1000000000L;
	}

	int ms = diff.tv_sec + diff.tv_nsec / 1e6;
	return ms;
}

int ltntstools_vbv_alloc(void **hdl, uint16_t pid, vbv_callback cb, void *userContext, struct vbv_decoder_profile_s *p)
{
	struct vbv_ctx_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx || !p) {
		return -1;
	}
	xorg_list_init(&ctx->pktList);
	pthread_mutex_init(&ctx->pktListMutex, NULL);

	ctx->decoder_profile = *p;

	struct vbv_decoder_profile_s *dp = &ctx->decoder_profile;
	if (dp->vbv_buffer_size == 0) {
		dp->vbv_buffer_size = DEFAULT_VBV_SIZE;
	}

	ctx->pid = pid;
	ctx->cb = cb;
	ctx->userContext = userContext;
	ctx->dts_hwm = 0;
	ctx->dts_lwm = INT64_MAX;
	ctx->dts_last = INT64_MAX;
	ctx->decoder_stc = INT64_MAX;
	ctx->verbose = 1;

	statsReset(ctx);

	/* Spawn a thread that manages the virtual decoder */
	pthread_create(&ctx->threadId, NULL, vbv_threadFunc, ctx);

	*hdl = ctx;
	return 0; /* Success */
}

void ltntstools_vbv_free(void *hdl)
{
	struct vbv_ctx_s *ctx = (struct vbv_ctx_s *)hdl;

	if (ctx->threadRunning) {
		ctx->threadTerminated = 0;
		ctx->threadTerminate = 1;
	}

	free(ctx);
}

int ltntstools_vbv_write(void *hdl, const struct ltn_pes_packet_s *pkt)
{
	struct vbv_ctx_s *ctx = (struct vbv_ctx_s *)hdl;
	if (!ctx || !pkt) {
		return -1; /* Missing mandatory args */
	}

	/* Attempting to write more payload into a full buffer should trigger an overflow. */
	if (addItem(ctx, pkt) < 0) {

	}

	return 0; /* Success*/
}


/* Simulate a virtual video decoder, pulling frames from a EBn (with VBV policy)
 * one frame at a time.
 * Create any UNDERFLOW conditions efficiently.
 */
static void * vbv_threadFunc(void *p)
{
	struct vbv_ctx_s *ctx = (struct vbv_ctx_s *)p;

	pthread_detach(ctx->threadId);
	ltnpthread_setname_np(ctx->threadId, "thread-vbv");

	ctx->threadTerminated = 0;
	ctx->threadRunning = 1;

	struct timespec next_time;

	struct timespec last_ooo_dts_time;
	int decoder_stc_rebased = 0;

	/* simulate HRD, don't drain vbv until this amount of content exists */
	/* 60% of a second into the VBV before we start to drain */
	int initial_cpb_removal_delay = (90000 / 100) * 60;
	int permit_vbv_drain = 0;

	while (!ctx->threadTerminate) {
		
		if (timesec_diff(next_time, last_ooo_dts_time) >= 50) {
			last_ooo_dts_time = next_time;

			/* Check the VBV and ensure DTS's are in order.
			 * Or'd OK for the VBV DTS array to go back in time (loop / reset)
			 */
		}
#if 0
		printf("dts_hwm %" PRIi64 " dts_lwm %" PRIi64 " diff %" PRIi64 "\n",
			ctx->dts_hwm, ctx->dts_lwm, ctx->dts_hwm - ctx->dts_lwm);
#endif
		if (permit_vbv_drain == 0 &&
			ctx->dts_lwm != INT64_MAX && ctx->dts_hwm != 0 &&
			ctx->dts_hwm - ctx->dts_lwm > initial_cpb_removal_delay)
		{
			permit_vbv_drain = 1;
			printf(MODULE_PREFIX "vbv initial level reached\n");
			clock_gettime(CLOCK_MONOTONIC, &next_time);
		}

		if (!permit_vbv_drain) {
			usleep(50 * 1000);
			continue;
		}

		struct pkt_item_s *item = NULL;
		pthread_mutex_lock(&ctx->pktListMutex);
		if (!xorg_list_is_empty(&ctx->pktList)) {
			item = xorg_list_first_entry(&ctx->pktList, struct pkt_item_s, list);
			if (item) {
				xorg_list_del(&item->list);
				ctx->usedBytes -= item->pkt.rawBufferLengthBytes;
			}
		}
		pthread_mutex_unlock(&ctx->pktListMutex);

		/* If the DTS wraps backwards, what? I don't think we care. */

		/* Cleanup */
		if (item) {
			if (decoder_stc_rebased == 0) {
				decoder_stc_rebased = 1;
				ctx->decoder_stc = item->pkt.DTS;
				if (ctx->decoder_stc == 0) {
					ctx->decoder_stc = item->pkt.PTS;
				}
				if (ctx->decoder_stc == 0) {
					decoder_stc_rebased = 0;
				}
			}

			double pct = ((double)ctx->usedBytes / (double)ctx->decoder_profile.vbv_buffer_size) * 100.0;

			if (ctx->verbose) {
				struct timeval ts;
				gettimeofday(&ts, NULL);
				printf("%d.%06d: decoder STC %14" PRIi64 ", got PTS %14" PRIi64 " DTS %14" PRIi64 " vbv: %8d / %5.2f%%\n",
					(int)ts.tv_sec, (int)ts.tv_usec,
					ctx->decoder_stc,
					item->pkt.PTS, item->pkt.DTS,
					ctx->usedBytes, pct);
			}
			free(item);

			if (pct <= 2.5 || pct >= 96.0) {
				raiseEvent(ctx, EVENT_VBV_FULLNESS_PCT);
			}
		} else {
			/* If the VBV is empty, what? */
			/* If the decoder wants a DTS and its not present in the list, what? */
			raiseEvent(ctx, EVENT_VBV_UNDERFLOW);
		}

		/* Track BPS used between DTS of one second and notify on this. */

		/* Have a haba daba too time... sleep a frame duration */
		uint32_t ns = framerateToUs(ctx->decoder_profile.framerate);
		next_time.tv_nsec += ns;
		while (next_time.tv_nsec >= 1000000000) {
			next_time.tv_nsec -= 1000000000;
			next_time.tv_sec += 1;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
		ctx->decoder_stc += framerateToTicks(ctx->decoder_profile.framerate);
	}
	ctx->threadRunning = 1;
	ctx->threadTerminated = 1;

	/* TODO: pthread detach else we'll cause a small leak in valgrind. */
	return NULL;
}
