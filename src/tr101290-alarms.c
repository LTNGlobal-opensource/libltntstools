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

void ltntstools_tr101290_alarm_raise(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	if (ev->raised == 0) {
		ev->raised = 1;
		gettimeofday(&ev->lastChanged, NULL);
	}
}

void ltntstools_tr101290_alarm_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	if (ev->raised == 1) {
		ev->raised = 0;
		gettimeofday(&ev->lastChanged, NULL);
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
