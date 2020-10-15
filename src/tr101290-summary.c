#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "libltntstools/tr101290.h"
#include "libltntstools/time.h"

#include "tr101290-types.h"

#define LOCAL_DEBUG 1

int ltntstools_tr101290_summary_get(void *hdl, struct ltntstools_tr101290_summary_item_s **item, int *itemCount)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	int count = _event_table_entry_count(s);

	struct ltntstools_tr101290_summary_item_s *arr = calloc(count, sizeof(struct ltntstools_tr101290_summary_item_s));
	if (!arr) {
		return -1;
	}

	pthread_mutex_lock(&s->mutex);
	for (int i = 0; i < count - 1; i++) {
		struct tr_event_s *ev = &s->event_tbl[i + 1];
		struct ltntstools_tr101290_summary_item_s *si = &arr[i];

		si->id = ev->id;
		si->enabled = ev->enabled;
		si->priorityNr = ev->priorityNr,
		si->last_update = ev->lastChanged;
		si->raised = ev->raised;
	}
	pthread_mutex_unlock(&s->mutex);

	*item = arr;
	*itemCount = count - 1;

	return 0;
}

void ltntstools_tr101290_summary_item_dprintf(int fd, struct ltntstools_tr101290_summary_item_s *si)
{
	dprintf(fd, "@%d.%6d -- Event P%d (%s): %s %s\n",
		(int)si->last_update.tv_sec,
		(int)si->last_update.tv_usec,
		si->priorityNr,
		si->enabled ? "enabled " : "disabled",
		si->raised ? "raised" : "clear ",
		ltntstools_tr101290_event_name_ascii(si->id));
}

int ltntstools_tr101290_summary_report_dprintf(void *hdl, int fd)
{
	struct ltntstools_tr101290_summary_item_s *arr = NULL;
	int count;

	int ret = ltntstools_tr101290_summary_get(hdl, &arr, &count);
	if (ret < 0)
		return ret;

	for (int i = 0; i < count; i++) {
		struct ltntstools_tr101290_summary_item_s *si = &arr[i];
		ltntstools_tr101290_summary_item_dprintf(fd, si);
	}

	free(arr);

	return 0;
}
