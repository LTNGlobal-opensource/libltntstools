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
#define TIMER_FIELD_DEFAULTS    .timerRequired = 0, .timerAlarmPeriodms = 0, .timerId = 0

struct tr_event_s tr_events_tbl[] =
{
	[E101290_UNDEFINED] = {
		.enabled = 0, .priorityNr = 1,
		E101290_UNDEFINED, "E101290_UNDEFINED",
		.raised = 0 /*  */, 1,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 },
		.autoClearAlarmAfterReport = 0,
		TIMER_FIELD_DEFAULTS,
	},

	/* Priority 1 */
	[E101290_P1_1__TS_SYNC_LOSS] = {
		.enabled = 1, .priorityNr = 1,
		E101290_P1_1__TS_SYNC_LOSS, "E101290_P1_1__TS_SYNC_LOSS",
		.raised = 0 /*  */, 1,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 },
		.autoClearAlarmAfterReport = 5, /* Seconds */
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P1_2__SYNC_BYTE_ERROR] = {
		.enabled = 1, .priorityNr = 1,
		E101290_P1_2__SYNC_BYTE_ERROR, "E101290_P1_2__SYNC_BYTE_ERROR",
		.raised = 0 /*  */, 1,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 },
		.autoClearAlarmAfterReport = 5, /* Seconds */
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P1_3__PAT_ERROR] = {
		.enabled = 1, .priorityNr = 1,
		E101290_P1_3__PAT_ERROR, "E101290_P1_3__PAT_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 },
		.autoClearAlarmAfterReport = 5, /* Seconds */
		.timerRequired = 1,
		.timerAlarmPeriodms = 500, /* As per the spec */
		.timerId = 0,
	},
	[E101290_P1_3a__PAT_ERROR_2] = {
		.enabled = 1, .priorityNr = 1,
		E101290_P1_3a__PAT_ERROR_2, "E101290_P1_3a__PAT_ERROR_2",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 },
		.autoClearAlarmAfterReport = 5, /* Seconds */
		.timerRequired = 0,
		.timerAlarmPeriodms = 500, /* As per the spec */
		.timerId = 0,
	},
	[E101290_P1_4__CONTINUITY_COUNTER_ERROR] = {
		.enabled = 1, .priorityNr = 1,
		E101290_P1_4__CONTINUITY_COUNTER_ERROR, "E101290_P1_4__CONTINUITY_COUNTER_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 },
		.autoClearAlarmAfterReport = 5, /* Seconds */
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P1_5__PMT_ERROR] = {
		.enabled = 0, .priorityNr = 1,
		E101290_P1_5__PMT_ERROR, "E101290_P1_5__PMT_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 },
		.autoClearAlarmAfterReport = 5, /* Seconds */
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P1_5a__PMT_ERROR_2] = {
		.enabled = 0, .priorityNr = 1,
		E101290_P1_5a__PMT_ERROR_2, "E101290_P1_5a__PMT_ERROR_2",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P1_6__PID_ERROR] = {
		.enabled = 0, .priorityNr = 1,
		E101290_P1_6__PID_ERROR, "E101290_P1_6__PID_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},

        /* Priority 2 */
	[E101290_P2_1__TRANSPORT_ERROR] = {
		.enabled = 0, .priorityNr = 2,
		E101290_P2_1__TRANSPORT_ERROR, "E101290_P2_1__TRANSPORT_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P2_2__CRC_ERROR] = {
		.enabled = 0, .priorityNr = 2,
		E101290_P2_2__CRC_ERROR, "E101290_P2_2__CRC_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P2_3__PCR_ERROR] = {
		.enabled = 0, .priorityNr = 2,
		E101290_P2_3__PCR_ERROR, "E101290_P2_3__PCR_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P2_3a__PCR_REPETITION_ERROR] = {
		.enabled = 0, .priorityNr = 2,
		E101290_P2_3a__PCR_REPETITION_ERROR, "E101290_P2_3a__PCR_REPETITION_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P2_4__PCR_ACCURACY_ERROR] = {
		.enabled = 0, .priorityNr = 2,
		E101290_P2_4__PCR_ACCURACY_ERROR, "E101290_P2_4__PCR_ACCURACY_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P2_5__PTS_ERROR] = {
		.enabled = 0, .priorityNr = 2,
		E101290_P2_5__PTS_ERROR, "E101290_P2_5__PTS_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},
	[E101290_P2_6__CAT_ERROR] = {
		.enabled = 0, .priorityNr = 2,
		E101290_P2_6__CAT_ERROR, "E101290_P2_6__CAT_ERROR",
		.raised = 0 /*  */, 0,
		{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 0 }, 0,
		TIMER_FIELD_DEFAULTS,
	},

        /* Priority 3 - Unsupported */
};

int _event_table_entry_count(struct ltntstools_tr101290_s *s)
{
	return sizeof(tr_events_tbl) / sizeof(struct tr_event_s);
}

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

	/* Decide whether we need to raise an alarm record for this. */
	int createUserAlarm = 1;
	struct timeval now;
	gettimeofday(&now, NULL);

	/* See if we're entitled to report this change, don't report more than once in a given window.
	 * For a given event, see how often is should be reported and skip over-reporting.
	 */
	struct timeval nextReport;
	timeradd(&ev->lastReported, &ev->reportInterval, &nextReport);

	/* In a startup condition, ensure lastreported is less that changed, but close. */
	if (ev->lastReported.tv_sec == 0)
		ev->lastReported.tv_sec = ev->lastChanged.tv_sec - 1;

	if (timercmp(&ev->lastReported, &ev->lastChanged, >=)) {
		createUserAlarm = 0;
	}

#if 0
printf("lc: %d.%d lr: %d.%d\n",
	ev->lastChanged.tv_sec, ev->lastChanged.tv_usec,
	ev->lastReported.tv_sec, ev->lastReported.tv_usec);

printf("create %d\n", createUserAlarm);
#endif
	return createUserAlarm;
}

