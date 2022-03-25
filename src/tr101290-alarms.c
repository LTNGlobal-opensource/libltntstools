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

void ltntstools_tr101290_alarm_raise_with_arg(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event, const char *msg)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	if (ev->raised == 0) {
		ev->raised = 1;
		gettimeofday(&ev->lastChanged, NULL);
	}
	sprintf(ev->arg, "%s", msg);

	/* Setup an timer, in N seconds this event goes into alarm again. */
	struct timeval interval = { ev->autoClearAlarmAfterReport / 1000, (ev->autoClearAlarmAfterReport * 1000) % 1000000 };
	struct timeval now;
	gettimeofday(&now, NULL);
	timeradd(&now, &interval, &ev->nextAlarm);
}

void ltntstools_tr101290_alarm_raise(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	if (ev->raised == 0) {
		ev->raised = 1;
		gettimeofday(&ev->lastChanged, NULL);
		ev->arg[0] = 0;
	}

	/* Setup an timer, in N seconds this event goes into alarm again. */
	struct timeval interval = { ev->autoClearAlarmAfterReport / 1000, (ev->autoClearAlarmAfterReport * 1000) % 1000000 };
	struct timeval now;
	gettimeofday(&now, NULL);
	timeradd(&now, &interval, &ev->nextAlarm);
}

void ltntstools_tr101290_alarm_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	if (ev->raised == 1) {
		/* If its already raised, don't allow a clear for N seconds */
		/* Clear events come in all the time, many times per second, allow the raise to linger. */
		struct timeval interval = { ev->autoClearAlarmAfterReport / 1000, (ev->autoClearAlarmAfterReport * 1000) % 1000000 };
		struct timeval now, future;
		gettimeofday(&now, NULL);
		timeradd(&interval, &ev->lastChanged, &future);
		if (timercmp(&now, &future, >= )) {
			ev->raised = 0;
			ev->lastChanged = now;
			ev->arg[0] = 0;
		}
	}

	/* Setup an timer, in N seconds this event goes into alarm again.
	 * Some other event will specifically need to clear it, before then.
	 */
	struct timeval interval = { ev->autoClearAlarmAfterReport / 1000, (ev->autoClearAlarmAfterReport * 1000) % 1000000 };
	struct timeval now;
	gettimeofday(&now, NULL);
	timeradd(&now, &interval, &ev->nextAlarm);
}

void ltntstools_tr101290_alarm_raise_all(struct ltntstools_tr101290_s *s)
{
    int count = _event_table_entry_count(s);
    for (int i = 1; i < count; i++) {
        struct tr_event_s *ev = &s->event_tbl[i];
		ltntstools_tr101290_alarm_raise(s, ev->id);
	}
}

void ltntstools_tr101290_event_dprintf(int fd, struct ltntstools_tr101290_alarm_s *alarm)
{
	dprintf(fd, "@%d.%6d -- Event P%d: %s %s - '%s'\n",
		(int)alarm->timestamp.tv_sec,
		(int)alarm->timestamp.tv_usec,
		alarm->priorityNr,
		ltntstools_tr101290_event_name_ascii(alarm->id),
		alarm->raised ? "raised" : "cleared",
		alarm->description);
}
