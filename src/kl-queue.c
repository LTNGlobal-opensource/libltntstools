/* Copyright Kernel Labs Inc 2017-2021. All Rights Reserved. */

#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/errno.h>
#include <libltntstools/kl-queue.h>

/* Initialize and track a queue of pointers in a FIFO type arrangement.
 * The implementation doesn't care about the pointer being tracked, and
 * make no attempt to "manage" its storage.
 * The implementation is best suited to application that push items
 * every 1ms or so, such to the overhead of calling malloc() on each push.
 * For projects that require usec accuracy, suggest pre-allocating
 * a list of free items.
 */

void klqueue_initialize(struct klqueue_s *q)
{
	memset(q, 0, sizeof(*q));
	xorg_list_init(&q->head);
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->item_add, NULL);
	pthread_cond_init(&q->item_remove, NULL);
};

uint64_t klqueue_count(struct klqueue_s *q)
{
	uint64_t count;
	pthread_mutex_lock(&q->mutex);
	count = q->item_count;
	pthread_mutex_unlock(&q->mutex);

	return count;
};

int klqueue_empty(struct klqueue_s *q)
{
	return klqueue_count(q) ? 0 : 1;
}

void klqueue_destroy(struct klqueue_s *q)
{
	/* Intensionally don't emit any signals */
	/* Intensionally leave the queue locked */

	int cnt = klqueue_count(q);
	if (cnt != 0) {
		fprintf(stderr, "%s(%p) Warning, leaking %d user pointers.\n", __func__, q, cnt);
		fprintf(stderr, "%s(%p) User needs to pop more before klqueue teardown.\n", __func__, q);
	}

	pthread_mutex_lock(&q->mutex);
	while (!xorg_list_is_empty(&q->head)) {
		struct klqueue_item_s *i = xorg_list_first_entry(&q->head, struct klqueue_item_s, list);
		xorg_list_del(&i->list);
		q->item_count--;

		/* We don't know what the caller put into the i->data, so we can't free it
		 * from a safety perspective, but we can free the allocations we've made.
		 * Valgrind will complain if the calluer is losing references.
		 */
		free(i);
	}
};

void klqueue_push(struct klqueue_s *q, void *item)
{
	struct klqueue_item_s *i = malloc(sizeof(*i));
	if (!i)
		return;

	i->data = item;

	pthread_mutex_lock(&q->mutex);
	xorg_list_append(&i->list, &q->head);
	q->item_count++;
	pthread_cond_signal(&q->item_add);
	pthread_mutex_unlock(&q->mutex);
}

/* blocking call that times out after n period */
int klqueue_pop_non_blocking(struct klqueue_s *q, int usec, void **item)
{
	int ret = -1;

	struct timespec abstime = { usec / 1000000, usec % 1000000 };
	pthread_mutex_lock(&q->mutex);

	if (xorg_list_is_empty(&q->head)) {
		ret = pthread_cond_timedwait(&q->item_add, &q->mutex, &abstime);
	} else
		ret = 0;

	if (ret == ETIMEDOUT) {
		pthread_mutex_unlock(&q->mutex);
		return ret;
	} else
	if (ret == 0) {
		/* Got an item */
		if (!xorg_list_is_empty(&q->head)) {
			struct klqueue_item_s *i = xorg_list_first_entry(&q->head, struct klqueue_item_s, list);
			xorg_list_del(&i->list);
			q->item_count--;
			*item = i->data;
			free(i);
			ret = 0;
		}
	} else {
		assert(0);
	}
	pthread_mutex_unlock(&q->mutex);
	if (ret == 0)
		pthread_cond_signal(&q->item_remove);

	return ret;
}
