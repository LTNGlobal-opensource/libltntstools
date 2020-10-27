#ifndef TR101290_TYPES_H
#define TR101290_TYPES_H

#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <libltntstools/stats.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "libltntstools/tr101290.h"
#include "libltntstools/time.h"

/* Set to 1 to include code that triggers test routines when certain files are present in /tmp.
 * Production builds should ALWAYS be sero to zero.
 */
#define ENABLE_TESTING 1

struct tr_event_s
{
	int enabled;
	int priorityNr;
	enum ltntstools_tr101290_event_e id;
	char name[64];

	int raised;
	int reportXX;
	struct timeval lastChanged;	/* When the raised state changes, we bump this timestamp. */
	struct timeval lastReported;	/* Last time we sent this alarm via the user callback. */
	struct timeval nextReportXX;
	struct timeval reportInterval;
	int autoClearAlarmAfterReport;  /* Automatically clear the alarm, if possible, 5 seconds after it returned to normal. */

	/* One timer per event, used to ensure time based events are properly tracked.
	 * Timers are used per event, and fire when an event that was supposed to occur every
	 * Nms doesn't occur, so the timer fires and an alarm condition is raised.
	 */
	int timerRequired; /* Boolean */
	int timerAlarmPeriodms; /* If the timer isn't cancelled within this period, it fires and an alarm is raised. */
	timer_t timerId;
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
	struct timeval now; /* Updated when a _write call arrives. */
	struct timeval lastWriteCall;

	struct ltntstools_stream_statistics_s streamStatistics;
	uint64_t PATCountLastTimer;
	uint64_t CCCounterLastWrite;

	void *smHandle; /* handle to a running PSIP stream modelling collector. */
};

#include "tr101290-events.h"
#include "tr101290-alarms.h"
#include "tr101290-timers.h"
#include "tr101290-p1.h"

#ifdef __cplusplus
};
#endif

#endif /* TR101290_TYPES_H */
