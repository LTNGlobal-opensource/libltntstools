/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include "libltntstools/ltntstools.h"
#include "xorg-list.h"

struct throughput_hires_item_s
{
	struct xorg_list list;

	uint64_t timestamp; /* Composite from struct timeval */
	uint32_t channel;   /* unique id, ex transport pid, probe id, sensor id. */
	int64_t  value_i64;
};

struct throughput_hires_context_s
{
	/* Two lists containing timestamp items.
	 * The lists are not sorted in any way.
	 */
	struct xorg_list itemsFree;
	struct xorg_list itemsBusy;
};

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

struct throughput_hires_item_s *itemAlloc()
{
	struct throughput_hires_item_s *item = calloc(1, sizeof(*item));
	return item;
}

void throughput_hires_free(void *hdl)
{
	struct throughput_hires_context_s *ctx = (struct throughput_hires_context_s *)hdl;

	while (!xorg_list_is_empty(&ctx->itemsFree)) {
		struct throughput_hires_item_s *item = xorg_list_first_entry(&ctx->itemsFree, struct throughput_hires_item_s, list);
		xorg_list_del(&item->list);
		free(item);
	}
	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct throughput_hires_item_s *item = xorg_list_first_entry(&ctx->itemsBusy, struct throughput_hires_item_s, list);
		xorg_list_del(&item->list);
		free(item);
	}

	free(ctx);
}

int throughput_hires_alloc(void **hdl, int itemsPerSecond)
{
	struct throughput_hires_context_s *ctx = calloc(1, sizeof(*ctx));

	xorg_list_init(&ctx->itemsFree);
	xorg_list_init(&ctx->itemsBusy);

	for (int i = 0; i < itemsPerSecond; i++) {
		struct throughput_hires_item_s *item = itemAlloc();
		xorg_list_append(&item->list, &ctx->itemsFree);
	}

	*hdl = ctx;

	return 0;
}

void throughput_hires_write_i64(void *hdl, uint32_t channel, int64_t value, struct timeval *ts)
{
	struct throughput_hires_context_s *ctx = (struct throughput_hires_context_s *)hdl;

	struct throughput_hires_item_s *item;
	if (xorg_list_is_empty(&ctx->itemsFree)) {
		for (int i = 0; i < 64; i++) {
			item = itemAlloc();
			xorg_list_append(&item->list, &ctx->itemsFree);
		}
	}
	
	item = xorg_list_first_entry(&ctx->itemsFree, struct throughput_hires_item_s, list);
	xorg_list_del(&item->list);

	if (ts)
		item->timestamp = makeTimestampFromTimeval(ts);
	else {
		item->timestamp = makeTimestampFromNow();
	}
	item->channel = channel;
	item->value_i64 = value;

	xorg_list_append(&item->list, &ctx->itemsBusy);
}

/* Expire any busy items older than ts, return a count of the number of expired items.
 * Passing NULL for a timestamp will expire anything older than 2 seconds old.
 */
int throughput_hires_expire(void *hdl, struct timeval *ts)
{
	struct throughput_hires_context_s *ctx = (struct throughput_hires_context_s *)hdl;

	int expired = 0;

	if (!xorg_list_is_empty(&ctx->itemsBusy)) {
		int64_t timestamp;
		if (ts)
			timestamp = makeTimestampFromTimeval(ts);
		else {
			timestamp = makeTimestampFrom2SecondAgo();
		}

		struct throughput_hires_item_s *e = NULL, *next = NULL;
		xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
			if (e->timestamp < timestamp) {
				expired++;
				xorg_list_del(&e->list);
				xorg_list_append(&e->list, &ctx->itemsFree);
			}
		}
	}

	return expired;
}

/* From is null then from default to 1 second ago.
 *  end is null then end details to now.
 */
int64_t throughput_hires_sumtotal_i64(void *hdl, uint32_t channel, struct timeval *from, struct timeval *to)
{
	struct throughput_hires_context_s *ctx = (struct throughput_hires_context_s *)hdl;

	int64_t total = 0;

	uint64_t begin, end;

	if (from)
		begin = makeTimestampFromTimeval(from);
	else
		begin = makeTimestampFrom1SecondAgo();

	if (to)
		end = makeTimestampFromTimeval(to);
	else
		end = makeTimestampFromNow();

	struct throughput_hires_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		if (e->channel == channel && e->timestamp >= begin && e->timestamp <= end) {
			total += e->value_i64;
		}
	}

	return total;
}

