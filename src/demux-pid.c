#include "demux-types.h"

#define LOCAL_DEBUG 0

#define ITEM_EXPIRE_MS 2000

uint32_t _itemAgeMs(struct demux_pid_pes_item_s *item, struct timeval *now)
{
	return ltn_timeval_subtract_ms(now, &item->ts);
}

/* Only call this wile holding the mutex */
void _itemFree(struct demux_pid_s *pid, struct demux_pid_pes_item_s *item)
{
	xorg_list_del(&item->list);
	ltn_pes_packet_free(item->pes);
	free(item);

	pid->pesListCount--;
}

void demux_pid_uninit(struct demux_pid_s *pid)
{
	pthread_mutex_lock(&pid->pesListMutex);
	if (pid->pe) {
		ltntstools_pes_extractor_free(pid->pe);
		pid->pe = NULL;
	}
	while (!xorg_list_is_empty(&pid->pesList)) {
		struct demux_pid_pes_item_s *e = xorg_list_first_entry(&pid->pesList, struct demux_pid_pes_item_s, list);
		_itemFree(pid, e);
	}
	// Leave locked
	// pthread_mutex_unlock(&pid->pesListMutex);
}

void demux_pid_init(struct demux_ctx_s *ctx, struct demux_pid_s *pid, uint16_t pidNr)
{
	pid->pidNr = pidNr;
	pid->pesListCount = 0;
	pid->payload = P_UNDEFINED;
	pid->ctx = ctx;

	pthread_mutex_init(&pid->pesListMutex, NULL);
	pthread_cond_init(&pid->pesListItemAdd, NULL);
	xorg_list_init(&pid->pesList);
}

void demux_pid_set_payload(struct demux_pid_s *pid, enum payload_e payload)
{
	pid->payload = payload;
}

/* PES Extractor callback */
void *demux_pid_pe_callback(void *userContext, struct ltn_pes_packet_s *pes)
{
	struct demux_pid_s *pid = (struct demux_pid_s *)userContext;
	struct demux_ctx_s *ctx = pid->ctx;
	struct timeval now;

	/* Alloc a context, put this PES object on a list */
	struct demux_pid_pes_item_s *item = malloc(sizeof(*item));
	if (!item) {
		ltn_pes_packet_free(pes);
		return NULL;
	}

	gettimeofday(&now, NULL);

	item->pes = pes;
	item->pid = pid;
	item->ts = now;

	pthread_mutex_lock(&pid->pesListMutex);
	xorg_list_append(&item->list, &pid->pesList);
	pid->pesListCount++;

	/* Expire anything on the list older than N ms.
	 * List is ordered by age descending, youngest end of list.
	 */
	struct demux_pid_pes_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &pid->pesList, list) {
		if (_itemAgeMs(e, &now) < ITEM_EXPIRE_MS) {
			break; /* Safe to break, because otime ordering guarantees */
		}

		/* Too old, expire it */
		_itemFree(pid, e);
	}
	pthread_mutex_unlock(&pid->pesListMutex);

	pthread_cond_signal(&pid->pesListItemAdd); /* TODO: Nobody is paying attension to this yet. */

	/* Give the caller a chance to deal with the pes if registered */
	if (ctx->callbacks && ctx->callbacks->cb_pes) {
		ctx->callbacks->cb_pes(ctx->userContext, pid->pidNr, pes);
	}

	/* We're holding onto the lifetime of the pes. We're not freeing it. */
	return NULL;
}