#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "libltntstools/tr101290.h"
#include "libltntstools/time.h"

#include "tr101290-types.h"

#define LOCAL_DEBUG 1

int64_t _timeval_to_ms(struct timeval *tv)
{
	return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}

static int didExperienceTransportLoss(struct ltntstools_tr101290_s *s)
{
	struct timeval now;
	gettimeofday(&now, NULL);

	/* Assume we have transport loss until the stats tell us different. */
	int lost = 1;

	struct timeval diff;
	timersub(&now, &s->lastWriteCall, &diff);

	int64_t ms = _timeval_to_ms(&diff);
	if (ms < 20) {
		lost = 0;
	} else {
#if LOCAL_DEBUG
		//printf("LOS for %" PRIi64 " ms\n", ms);
#endif
	}

	return lost;
}

void *ltntstools_tr101290_threadFunc(void *p)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)p;

	s->threadRunning = 1;

	/* Raise alert on every event */
	ltntstools_tr101290_alarm_raise_all(s);

	struct timeval now;
	while (!s->threadTerminate) {
		usleep(10 * 1000);
		gettimeofday(&now, NULL);

		int conditionLOS = didExperienceTransportLoss(s);
		if (conditionLOS) {
			ltntstools_tr101290_alarm_raise(s, E101290_P1_1__TS_SYNC_LOSS);
		} else {
			/* If the period of time between the last report and this clear is more than
			 * five seconds, automatically clear the alarm.
			 */

			struct tr_event_s *ev = &s->event_tbl[E101290_P1_1__TS_SYNC_LOSS];

			if (ev->autoClearAlarmAfterReport) {
				struct timeval interval = { ev->autoClearAlarmAfterReport, 0 };
				struct timeval final;
				timeradd(&ev->lastReported, &interval, &final);
			
				if (timercmp(&now, &final, >= )) {
					ltntstools_tr101290_alarm_clear(s, ev->id);
				}
			}
		}

		/* For each possible event, determine if we need to build and alarm
		 * record to inform the user (via callback.
		 */
		for (int i = 1; i < (int)E101290_MAX; i++) {
			struct tr_event_s *ev = &s->event_tbl[i];
			if (ev->enabled == 0)
				continue;

			/* Find all events we should be reproting on,  */
			if (ltntstools_tr101290_event_should_report(s, ev->id)) {
				ev->lastReported = now;

				/* Mark the last reported time slight int the future, to avoid duplicate
				 * reports within a few useconds of each other
				 */
				struct timeval interval10ms = { 0, 1 * 1000 };
				timeradd(&now, &interval10ms, &ev->lastReported);

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

			}
#if 0
			if (ev->autoClearAlarmAfterReport)
				_tr101290_event_clear(s, ev->id);
#endif
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

	ltntstools_pid_stats_reset(&s->streamStatistics);

	ltntstools_pat_parser_alloc(&s->patParser, s);

	int count = _event_table_entry_count(s);
	for (int i = 0; i < count; i++) {
                struct tr_event_s *ev = &s->event_tbl[i];

		if (ev->enabled && ev->timerRequired) {
			int ret = ltntstools_tr101290_timers_create(s, ev);
			if (ret < 0) {
				fprintf(stderr, "%s() Unable to create timer\n", __func__);
				exit(1);
			}
			ret = ltntstools_tr101290_timers_arm(s, ev);
			if (ret < 0) {
				fprintf(stderr, "%s() Unable to arm timer\n", __func__);
				exit(1);
			}

		}
	}

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

	struct timeval now;
	gettimeofday(&now, NULL);

#if LOCAL_DEBUG
	//printf("%s(%d)\n", __func__, packetCount);
#endif

#if ENABLE_TESTING
	FILE *fh = fopen("/tmp/droppayload", "rb");
	if (fh) {
		fclose(fh);
		return packetCount;
	}
#endif

	pthread_mutex_lock(&s->mutex);

	/* The thread needs to understand how frequently we're getting write calls. */
	gettimeofday(&s->lastWriteCall, NULL);

	ltntstools_pid_stats_update(&s->streamStatistics, buf, packetCount);

	p1_write(s, buf, packetCount);

	pthread_mutex_unlock(&s->mutex);

	return packetCount;
}

