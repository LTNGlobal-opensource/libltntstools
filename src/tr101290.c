#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "libltntstools/tr101290.h"
#include "libltntstools/time.h"

#define LOCAL_DEBUG 1

struct tr_event_s
{
	int enabled;
	int priorityNr;
	enum ltntstools_tr101290_event_e id;
	char name[64];

	int raised;
	int report;
	struct timeval lastRaised;
	struct timeval lastReported;
	struct timeval nextReport;
	struct timeval reportInterval;
	int autoClearAlarmAfterReport;

} tr_events_tbl[] =
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

struct ltntstools_tr101290_s
{
	pthread_t threadId;
	int threadTerminate, threadTerminated, threadRunning;

	ltntstools_tr101290_notification cb_notify;
	void *userContext;

	pthread_mutex_t mutex;
	struct tr_event_s *event_tbl; /* A cloned and modified version of the tr_events_tbl */

	/* Alarm list for reporting to user */
	int alarmCount;
	struct ltntstools_tr101290_alarm_s *alarm_tbl;

	/* Vars to help track alarm stats */
	int consecutiveSyncBytes;
};

static int ltntstools_tr101290_event_should_report(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event);
static void _tr101290_event_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event);
static void ltntstools_tr101290_alarm_raise(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event);
static void ltntstools_tr101290_alarm_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event);

void *ltntstools_tr101290_threadFunc(void *p)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)p;

	s->threadRunning = 1;

	while (!s->threadTerminate) {
		usleep(10 * 1000);

		for (int i = 1; i < (int)E101290_MAX; i++) {
			struct tr_event_s *ev = &s->event_tbl[i];
			if (ev->enabled == 0)
				continue;

			/* MMM: Find all events we should be reproting on,  */
			if (ltntstools_tr101290_event_should_report(s, ev->id)) {
#if LOCAL_DEBUG
				printf("%s(?, %s) will report\n", __func__, ltntstools_tr101290_event_name_ascii(ev->id));
#endif
				/* Create an alarm record, the thread will pass is to the caller later. */
				s->alarm_tbl = realloc(s->alarm_tbl, (s->alarmCount + 1) * sizeof(struct ltntstools_tr101290_alarm_s));

				struct ltntstools_tr101290_alarm_s *alarm = &s->alarm_tbl[s->alarmCount];
				alarm->id = ev->id;
				alarm->priorityNr = ltntstools_tr101290_event_priority(ev->id);
				alarm->raised = ev->raised;
				gettimeofday(&alarm->timestamp, NULL);
				strcpy(alarm->description, "Alarm raised");
				s->alarmCount++;

				/* Decide the next time we should report for this event condition. */
				struct timeval now;
				gettimeofday(&now, NULL);

				if (ev->report && ev->raised == 0) {
					/* Don't raise it a second time around, if its a clear event. */
					struct timeval theDistantFuture = { 0x0fffffff, 0 };
					timeradd(&now, &theDistantFuture, &ev->nextReport);
				} else {
					timeradd(&now, &ev->reportInterval, &ev->nextReport);
				}
			}

			if (ev->autoClearAlarmAfterReport)
				_tr101290_event_clear(s, ev->id);
		}

		/* Pass any alarms to the callback */
		pthread_mutex_lock(&s->mutex);

		struct ltntstools_tr101290_alarm_s *cpy = NULL;
		int count = s->alarmCount;
		int bytes = count * sizeof(struct ltntstools_tr101290_alarm_s);
		if (bytes) {
			cpy = malloc(bytes);
			memcpy(cpy, s->alarm_tbl, bytes);
		}

		pthread_mutex_unlock(&s->mutex);

		if (bytes && cpy && s->cb_notify) {
			s->cb_notify(s->userContext, cpy, count);
			s->alarmCount = 0;
		}
	}

	s->threadRunning = 0;
	s->threadTerminated = 1;
	return NULL;
}

int ltntstools_tr101290_alloc(void **hdl, ltntstools_tr101290_notification cb_notify, void *userContext)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)calloc(1, sizeof(*s));
	s->event_tbl = malloc(sizeof(tr_events_tbl));
	memcpy(s->event_tbl, tr_events_tbl, sizeof(tr_events_tbl));
	s->userContext = userContext;
	s->cb_notify = cb_notify;

	pthread_mutex_init(&s->mutex, NULL);
	*hdl = s;

	return pthread_create(&s->threadId, NULL, ltntstools_tr101290_threadFunc, s);
}

void ltntstools_tr101290_free(void *hdl)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;
	if (s->threadRunning) {
		s->threadTerminate = 1;
		while (!s->threadTerminated)
			usleep(1 * 1000);
	}
	free(s->event_tbl);
	free(s);
}

static void ltntstools_tr101290_alarm_raise(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	ev->raised = 1;
	ev->report = 1;
}

static void ltntstools_tr101290_alarm_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	ev->raised = 0;
	ev->report = 1;
}

ssize_t ltntstools_tr101290_write(void *hdl, const uint8_t *buf, size_t packetCount)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

#if LOCAL_DEBUG
	//printf("%s(%d)\n", __func__, packetCount);
#endif
	pthread_mutex_lock(&s->mutex);

	/* P1.2 - Sync Byte Error, sync byte != 0x47 */
	for (int i = 0; i < packetCount; i += 188) {
		if (buf[i] != 0x47) {
			/* Raise */
			s->consecutiveSyncBytes = 0;
			ltntstools_tr101290_alarm_raise(s, E101290_P1_1__TS_SYNC_LOSS);
			ltntstools_tr101290_alarm_raise(s, E101290_P1_2__SYNC_BYTE_ERROR);
		} else
			s->consecutiveSyncBytes++;
	}

	if (s->consecutiveSyncBytes > 3 && s->consecutiveSyncBytes <= 7) {
		/* Clear Alarm */
		ltntstools_tr101290_alarm_clear(s, E101290_P1_1__TS_SYNC_LOSS);
		ltntstools_tr101290_alarm_clear(s, E101290_P1_2__SYNC_BYTE_ERROR);
	}
	if (s->consecutiveSyncBytes >= 50000) {
		/* We never want the int to wrap back to zero. Once we're a certain size,
		 * our wrap point needs to clear the reset of the 3->7 alarm condition above.
		 */
		s->consecutiveSyncBytes = 16; /* Stay clear of the window where sync byte is cleared. */
	}

	pthread_mutex_unlock(&s->mutex);

	return packetCount;
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

static void _tr101290_event_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
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
static int ltntstools_tr101290_event_should_report(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
	struct tr_event_s *ev = &s->event_tbl[event];

	//printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));

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
	} else {
#if LOCAL_DEBUG
		printf("%s(?, %s) - create alarm\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	}

	return createUserAlarm;
}

