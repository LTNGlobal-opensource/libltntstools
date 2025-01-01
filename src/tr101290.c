#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

#include "libltntstools/ltntstools.h"

#include "tr101290-types.h"

#define LOCAL_DEBUG 0

int ltntstools_tr101290_log_append(struct ltntstools_tr101290_s *s, int addTimestamp, const char *format, ...)
{
	int ret = 0;

	pthread_mutex_lock(&s->logMutex);
	if (!s->logFilename) {
		pthread_mutex_unlock(&s->logMutex);
		return ret; /* Silently discard message */
	}

	char buf[2048];
	buf[0] = 0;

	if (addTimestamp) {
		time_t now = time(NULL);
		struct tm *whentm = localtime(&now);
		char ts[64];
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", whentm);

		sprintf(buf, "%s: ", ts);
	}

    va_list vl;
    va_start(vl, format);
    vsprintf(&buf[strlen(buf)], format, vl);
    va_end(vl);

	sprintf(buf + strlen(buf), "\n");
    //printf("%s", buf);

	FILE *fh = fopen(s->logFilename, "a+");
	if (fh) {

		/* we're a super user, obtain any SUDO uid and change file ownership to it - if possible. */
		if (!s->logOwnershipOK && fh && getuid() == 0 && getenv("SUDO_UID") && getenv("SUDO_GID")) {
			s->logOwnershipOK = 1;
			uid_t o_uid = atoi(getenv("SUDO_UID"));
			gid_t o_gid = atoi(getenv("SUDO_GID"));

			if (chown(s->logFilename, o_uid, o_gid) != 0) {
				/* Error */
				fprintf(stderr, "Error changing %s ownership to uid %d gid %d, ignoring\n",
					s->logFilename, o_uid, o_gid);
			}
		}

		fputs(buf, fh);
		fclose(fh);
	} else {
		ret = -1;
	}

	pthread_mutex_unlock(&s->logMutex);

	return ret;
}

/* Write the state of all enabled events to the logger.
 * In terms of serialization, make sure this is ONLY called
 * from the thread, to avoid an ugly looking log.
 */
static void ltntstools_tr101290_log_summary(struct ltntstools_tr101290_s *s)
{
	char buf[256];

	for (int i = 1; i < (int)E101290_MAX; i++) {
		struct tr_event_s *ev = &s->event_tbl[i];
		if (ev->enabled == 0)
			continue;

		sprintf(buf, "%-40s - Status %s ", ltntstools_tr101290_event_name_ascii(ev->id), ev->raised ? " raised" : "cleared");

		if (strlen(ev->arg)) {
			if (ev->raised) {
				ltntstools_tr101290_log_append(s, 1, "%s [ %s ]", buf, ev->arg);
			} else {
				ltntstools_tr101290_log_append(s, 1, buf);
			}
		} else {
			ltntstools_tr101290_log_append(s, 1, buf);
		}

	}
}

/*
 * A general event loop that raises and clears alarms based on various
 * conditions.
 * Alarms are passed to the upper layers by way of a callback.
 * The users callback could block this thread, which isn't nice.
 * 
 * Alarms can be raised and cleared by this thread, or by
 * calls to ltntstools_tr101290_write().
 */
void *ltntstools_tr101290_threadFunc(void *p)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)p;
	struct timeval nextSummaryTime = { 0 };

#define PERIODIC_STATUS_REPORT_SECS 60
	int reportPeriod = PERIODIC_STATUS_REPORT_SECS;
	s->threadRunning = 1;

	/* Raise alert on every event */
	ltntstools_tr101290_alarm_raise_all(s);

	struct timeval now;
	while (!s->threadTerminate) {
		usleep(10 * 1000);
		gettimeofday(&now, NULL);

		/* For each possible event, determine if we need to build and alarm
		 * record to inform the user (via callback.
		 */
		for (int i = 1; i < (int)E101290_MAX; i++) {
			struct tr_event_s *ev = &s->event_tbl[i];
			if (ev->enabled == 0)
				continue;

			p2_process_p2_2(s);
#if 1
			if (timercmp(&now, &nextSummaryTime, >= )) {
				struct timeval interval = { reportPeriod, 0 };
				timeradd(&now, &interval, &nextSummaryTime);

				ltntstools_tr101290_log_append(s, 1, "Periodic Status Report --------------------------------------------------------------------------------");
				ltntstools_tr101290_log_summary(s);
				ltntstools_tr101290_log_append(s, 1, "-------------------------------------------------------------------------------------------------------");
			}
#endif
			/* Find all events we should be reporting on,  */
			if (ltntstools_tr101290_event_should_report(s, ev->id, &now)) {

				/* Mark the last reported time slight int the future, to avoid duplicate
				 * reports within a few seconds of each other
				 */
				struct timeval interval10ms = { 0, 1 * 1000 };
				timeradd(&now, &interval10ms, &ev->lastReported);

#if LOCAL_DEBUG
				printf("%s(?, %s) will report\n", __func__, ltntstools_tr101290_event_name_ascii(ev->id));
#endif
				/* Create an alarm record, the thread will pass is to the caller later. */
				s->alarm_tbl = realloc(s->alarm_tbl, (s->alarmCount + 1) * sizeof(struct ltntstools_tr101290_alarm_s));
				if (!s->alarm_tbl)
					continue;

				struct ltntstools_tr101290_alarm_s *alarm = &s->alarm_tbl[s->alarmCount];
				alarm->id = ev->id;
				alarm->priorityNr = ltntstools_tr101290_event_priority(ev->id);
				alarm->raised = ev->raised;
				alarm->timestamp = now;

				time_t when = alarm->timestamp.tv_sec;
				struct tm *whentm = localtime(&when);
				char ts[64];
				strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", whentm);

                               sprintf(alarm->description, "%s",
                                       ltntstools_tr101290_event_name_ascii(alarm->id));
				strcpy(alarm->arg, ev->arg);

				if (strlen(alarm->arg)) {
					if (alarm->raised) {
                                               ltntstools_tr101290_log_append(s, 0, "%s: %-40s [ %s ] - Alarm  raised", ts, alarm->description, alarm->arg);
					} else {
                                               ltntstools_tr101290_log_append(s, 0, "%s: %-40s - Alarm cleared", ts, alarm->description);
					}
				} else {
                                       ltntstools_tr101290_log_append(s, 0, "%s: %-40s - %s", ts, alarm->description, alarm->raised ? " raised" : "cleared");
				}			
				s->alarmCount++;
			}

			/* All events naturally want to be in a raise state,
			 * in the event of no data, or something holding them clear.
			 */
			if (timercmp(&now, &ev->nextAlarm, >= )) {
				ltntstools_tr101290_alarm_raise(s, ev->id, &now);
			}

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
			/* The user is responsible for the lifespan of this object. */
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
	pthread_mutex_init(&s->logMutex, NULL);

	s->consecutiveSyncErrors = 0;

	ltn_histogram_alloc_video_defaults(&s->h1, "write arrival latency");

	ltntstools_pid_stats_reset(&s->streamStatistics);

	ltntstools_streammodel_alloc(&s->smHandle, s);
	ltntstools_streammodel_enable_tr101290_section_checks(s->smHandle, (ltntstools_streammodel_callback)p2_streammodel_callback);

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

	ltntstools_tr101290_log_append(s, 1, "TR101290 Logging stopped");

	pthread_mutex_destroy(&s->mutex);
	pthread_mutex_destroy(&s->logMutex);

	fprintf(stderr, "TR101290: Freeing Event Table\n");
	ltn_histogram_free(s->h1);

	if (s->alarm_tbl)
		free(s->alarm_tbl);
	s->alarm_tbl = NULL;
	if (s->event_tbl)
		free(s->event_tbl);
	s->event_tbl = NULL;
	if (s->logFilename) {
		free(s->logFilename);
		s->logFilename = NULL;
	}

	free(s);
}

ssize_t ltntstools_tr101290_write(void *hdl, const uint8_t *buf, size_t packetCount, struct timeval *timestamp)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	if (timestamp) {
		s->now = *timestamp;
	} else {
		gettimeofday(&s->now, NULL);
	}

	ltn_histogram_interval_update(s->h1, timestamp);
	//ltn_histogram_interval_print(STDOUT_FILENO, s->h1, 10);

#if ENABLE_TESTING
	FILE *fh = fopen("/tmp/droppayload", "rb");
	if (fh) {
		fclose(fh);
		return packetCount;
	}
#endif

	pthread_mutex_lock(&s->mutex);

	/* The thread needs to understand how frequently we're getting write calls. */
	s->lastWriteCall = s->now;

	s->preTEIErrors = ltntstools_pid_stats_stream_get_tei_errors(&s->streamStatistics);
	s->preScrambledCount = ltntstools_pid_stats_stream_get_scrambled_count(&s->streamStatistics);

	/* Update general stream statistics, packet loss, CC's, birates etc. */
	ltntstools_pid_stats_update(&s->streamStatistics, buf, packetCount);

	/* Pass all of the packets to the P1 analysis layer. */
	p1_write(s, buf, packetCount, &s->now);

	/* Pass all of the packets to the P1 analysis layer. */
	p2_write(s, buf, packetCount, &s->now);

	pthread_mutex_unlock(&s->mutex);

	return packetCount;
}

int ltntstools_tr101290_log_enable(void *hdl, const char *afname)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	pthread_mutex_lock(&s->logMutex);
	if (s->logFilename) {
		pthread_mutex_unlock(&s->logMutex);
		return -1; /* Invalid to change the log directory once set. */
	}

	free(s->logFilename);
	s->logFilename = strdup(afname);

	pthread_mutex_unlock(&s->logMutex);

	int ret = ltntstools_tr101290_log_append(s, 1, "TR101290 Logging started");

	return ret;
}

int ltntstools_tr101290_log_rotate(void *hdl)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	pthread_mutex_lock(&s->logMutex);
	/* TODO: */
	pthread_mutex_unlock(&s->logMutex);

	return -1;
}

int ltntstools_tr101290_reset_alarms(void *hdl)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)hdl;

	pthread_mutex_lock(&s->logMutex);
	ltntstools_tr101290_alarm_raise_all(s);
	pthread_mutex_unlock(&s->logMutex);

	return 0; /* Success */
}
