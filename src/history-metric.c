/* Copyright LiveTimeNet, Inc. 2026. All Rights Reserved. */

#include <stdlib.h>
#include <string.h>

#include "libltntstools/history-metric.h"
#include "xorg-list.h"

struct ltntstools_history_metric_s *ltntstools_history_metric_alloc(time_t now, uint64_t value)
{
	struct ltntstools_history_metric_s *m = (struct ltntstools_history_metric_s *)malloc(sizeof(*m));
	if (m) {
		m->ts = now;
		m->count = value;
	}
	return m;
}

void ltntstools_history_metric_free(struct ltntstools_history_metric_s *m)
{
	free(m);
}

int ltntstools_history_metric_collection_init(struct ltntstools_history_metric_collection_s *c, const char *name)
{
	if (!name || !name) {
		return -1; /* Failed */
	}

	xorg_list_init(&c->list);
	pthread_mutex_init(&c->lock, NULL);
	c->name = strdup(name);

	return 0; /* Success */
}

struct ltntstools_history_metric_collection_s *ltntstools_history_metric_collection_alloc(const char *name)
{
	if (!name) {
		return NULL;
	}

	struct ltntstools_history_metric_collection_s *c = (struct ltntstools_history_metric_collection_s *)malloc(sizeof(*c));
	if (c) {
		ltntstools_history_metric_collection_init(c, name);
	}
	return c;
}

void ltntstools_history_metric_collection_free(struct ltntstools_history_metric_collection_s *c)
{
	struct ltntstools_history_metric_s *m = NULL;

	pthread_mutex_lock(&c->lock);
	while (!xorg_list_is_empty(&c->list)) {
		m = xorg_list_first_entry(&c->list, struct ltntstools_history_metric_s, list);
		if (m) {
			xorg_list_del(&m->list);
			ltntstools_history_metric_free(m);
		}
	}
	pthread_mutex_unlock(&c->lock);

	free((char *)c->name);
	free(c);
}

void ltntstools_history_metric_collection_add(struct ltntstools_history_metric_collection_s *c,
	struct ltntstools_history_metric_s *m)
{
	if (c && m) {
		/* Insert to the head of the queue */
		pthread_mutex_lock(&c->lock);
		xorg_list_add(&m->list, &c->list); /* Newest entry always on top, ordered by age */
		pthread_mutex_unlock(&c->lock);

		/* Don't keep anything older than 25hrs */
		/* No _reverse() enumeration that's safe, go forward instead. */
		struct ltntstools_history_metric_s *e = NULL, *next = NULL;
		time_t window = time(NULL) - (25 * 60 * 60);

		pthread_mutex_lock(&c->lock);
		xorg_list_for_each_entry_safe(e, next, &c->list, list) {
			if (e->ts < window) {
				xorg_list_del(&m->list);
				ltntstools_history_metric_free(m);
			}
		}
		pthread_mutex_unlock(&c->lock);
	}
}

void ltntstools_history_metric_collection_reset(struct ltntstools_history_metric_collection_s *c)
{
	if (!c) {
		return;
	}

	pthread_mutex_lock(&c->lock);
	struct ltntstools_history_metric_s *m = NULL;
	while (!xorg_list_is_empty(&c->list)) {
		m = xorg_list_first_entry(&c->list, struct ltntstools_history_metric_s, list);
		if (m) {
			xorg_list_del(&m->list);
			ltntstools_history_metric_free(m);
		}
	}
	pthread_mutex_unlock(&c->lock);
}

int ltntstools_history_metric_collection_count_until(struct ltntstools_history_metric_collection_s *c, time_t window, uint64_t *result)
{
	if (!c) {
		return -1; /* Failed */
	}

	time_t now = time(NULL);

	uint64_t total = 0;
	struct ltntstools_history_metric_s *m = NULL;

	pthread_mutex_lock(&c->lock);
	xorg_list_for_each_entry(m, &c->list, list) {
		if (now <= window) {
			total += m->count;
		}
	}
	pthread_mutex_unlock(&c->lock);

	*result = total;

	return 0; /* Success */
}

int ltntstools_history_metric_collection_count_until_1hr(struct ltntstools_history_metric_collection_s *c, uint64_t *result)
{
	if (!c) {
		return -1; /* Failed */
	}
	return ltntstools_history_metric_collection_count_until(c, time(NULL) - 3600, result);
}

int ltntstools_history_metric_collection_count_until_24hr(struct ltntstools_history_metric_collection_s *c, uint64_t *result)
{
	if (!c) {
		return -1; /* Failed */
	}
	return ltntstools_history_metric_collection_count_until(c, time(NULL) - 86400, result);
}
