#ifndef TR101290_TYPES_H
#define TR101290_TYPES_H

#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "libltntstools/tr101290.h"
#include "libltntstools/time.h"

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

#include "tr101290-events.h"
#include "tr101290-alarms.h"

#ifdef __cplusplus
};
#endif

#endif /* TR101290_TYPES_H */
