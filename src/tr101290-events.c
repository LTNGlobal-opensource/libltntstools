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

#define LOCAL_DEBUG 0

struct tr_event_s tr_events_tbl[] =
{
	[E101290_UNDEFINED]{ 0, 1, E101290_UNDEFINED, "E101290_UNDEFINED", 0, 0, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0 },

	/* Priority 1 */
	[E101290_P1_1__TS_SYNC_LOSS]{
		1, 1,
		E101290_P1_1__TS_SYNC_LOSS, "E101290_P1_1__TS_SYNC_LOSS",
		1 /* Default raised */, 1,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P1_2__SYNC_BYTE_ERROR]{
		1, 1,
		E101290_P1_2__SYNC_BYTE_ERROR, "E101290_P1_2__SYNC_BYTE_ERROR",
		1 /* Default raised */, 1,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P1_3__PAT_ERROR]{
		1, 1,
		E101290_P1_3__PAT_ERROR, "E101290_P1_3__PAT_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P1_3a__PAT_ERROR_2]{
		1, 1,
		E101290_P1_3a__PAT_ERROR_2, "E101290_P1_3a__PAT_ERROR_2",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P1_4__CONTINUITY_COUNTER_ERROR]{
		1, 1,
		E101290_P1_4__CONTINUITY_COUNTER_ERROR, "E101290_P1_4__CONTINUITY_COUNTER_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P1_5__PMT_ERROR]{
		1, 1,
		E101290_P1_5__PMT_ERROR, "E101290_P1_5__PMT_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P1_5a__PMT_ERROR_2]{
		1, 1,
		E101290_P1_5a__PMT_ERROR_2, "E101290_P1_5a__PMT_ERROR_2",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P1_6__PID_ERROR]{
		1, 1,
		E101290_P1_6__PID_ERROR, "E101290_P1_6__PID_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},

        /* Priority 2 */
	[E101290_P2_1__TRANSPORT_ERROR]{
		1, 2,
		E101290_P2_1__TRANSPORT_ERROR, "E101290_P2_1__TRANSPORT_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P2_2__CRC_ERROR]{
		1, 2,
		E101290_P2_2__CRC_ERROR, "E101290_P2_2__CRC_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P2_3__PCR_ERROR]{
		1, 2,
		E101290_P2_3__PCR_ERROR, "E101290_P2_3__PCR_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P2_3a__PCR_REPETITION_ERROR]{
		1, 2,
		E101290_P2_3a__PCR_REPETITION_ERROR, "E101290_P2_3a__PCR_REPETITION_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P2_4__PCR_ACCURACY_ERROR]{
		1, 2,
		E101290_P2_4__PCR_ACCURACY_ERROR, "E101290_P2_4__PCR_ACCURACY_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P2_5__PTS_ERROR]{
		1, 2,
		E101290_P2_5__PTS_ERROR, "E101290_P2_5__PTS_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},
	[E101290_P2_6__CAT_ERROR]{
		1, 2,
		E101290_P2_6__CAT_ERROR, "E101290_P2_6__CAT_ERROR",
		0 /* Default not raised */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0
	},

        /* Priority 3 - Unsupported */
};

struct tr_event_s *ltntstools_tr101290_event_table_copy()
{
	struct tr_event_s *arr = malloc(sizeof(tr_events_tbl));
	memcpy(arr, tr_events_tbl, sizeof(tr_events_tbl));
	return arr;
}


const char *ltntstools_tr101290_event_name_ascii(enum ltntstools_tr101290_event_e event)
{
	if (event >= E101290_MAX)
		return tr_events_tbl[E101290_UNDEFINED].name;

	return tr_events_tbl[event].name;
}

int ltntstools_tr101290_event_priority(enum ltntstools_tr101290_event_e event)
{
	if (event >= E101290_MAX)
		return tr_events_tbl[E101290_UNDEFINED].priorityNr;

	return tr_events_tbl[event].priorityNr;
}

int ltntstools_tr101290_event_processing_enable(void *hdl, enum ltntstools_tr101290_event_e event)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	if (event >= E101290_MAX)
		return -1;

	pthread_mutex_lock(&s->mutex);
	s->event_tbl[event].enabled = 1;
	pthread_mutex_unlock(&s->mutex);

	return 0;
}

int ltntstools_tr101290_event_processing_disable(void *hdl, enum ltntstools_tr101290_event_e event)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	if (event >= E101290_MAX)
		return -1;

	pthread_mutex_lock(&s->mutex);
	s->event_tbl[event].enabled = 0;
	pthread_mutex_unlock(&s->mutex);

	return 0;
}

int ltntstools_tr101290_event_processing_disable_all(void *hdl)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	pthread_mutex_lock(&s->mutex);

	for (int i = 1; i < (int)E101290_MAX; i++)
		s->event_tbl[i].enabled = 0;

	pthread_mutex_unlock(&s->mutex);

	return 0;
}

int ltntstools_tr101290_event_processing_enable_all(void *hdl)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	pthread_mutex_lock(&s->mutex);

	for (int i = 1; i < (int)E101290_MAX; i++)
		s->event_tbl[i].enabled = 1;

	pthread_mutex_unlock(&s->mutex);

	return 0;
}

void _tr101290_event_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	s->event_tbl[event].raised = 0;
}

int ltntstools_tr101290_event_clear(void *hdl, enum ltntstools_tr101290_event_e event)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	if (event >= E101290_MAX)
		return -1;

	pthread_mutex_lock(&s->mutex);
	_tr101290_event_clear(s, event);
	pthread_mutex_unlock(&s->mutex);

	return 0;
}

/* For a given event in the user context, see if its enable, expected to be reported,
 * and the report window dictates that we should be reproting it, react accordingly.
 */
int ltntstools_tr101290_event_should_report(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
	struct tr_event_s *ev = &s->event_tbl[event];

#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif

	if (ev->enabled == 0) {
		return 0;
	}
	if (ev->report == 0) {
		return 0;
	}

	/* Decide whether we need to raise an alarm record for this. */
	int createUserAlarm = 1;
	struct timeval now;
	gettimeofday(&now, NULL);

	if (timercmp(&now, &ev->nextReport, <)) {
		createUserAlarm = 0;
	}

	return createUserAlarm;
}

