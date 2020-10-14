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

void *ltntstools_tr101290_threadFunc(void *p)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)p;

	s->threadRunning = 1;

	while (!s->threadTerminate) {
		usleep(10 * 1000);

		/* For each possible event, determine if we need to build and alarm
		 * record to inform the user (via callback.
		 */
		for (int i = 1; i < (int)E101290_MAX; i++) {
			struct tr_event_s *ev = &s->event_tbl[i];
			if (ev->enabled == 0)
				continue;

			/* Find all events we should be reproting on,  */
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

				time_t when = alarm->timestamp.tv_sec;
				struct tm *whentm = localtime(&when);
				char ts[64];
				strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", whentm);

				sprintf(alarm->description, "%s: Alarm %s", ts, alarm->raised ? "raised" : "cleared");

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
	s->event_tbl = ltntstools_tr101290_event_table_copy();
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

