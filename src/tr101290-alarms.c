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
#include "tr101290-events.h"
#include "tr101290-alarms.h"

#define LOCAL_DEBUG 1

void ltntstools_tr101290_alarm_raise(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	ev->raised = 1;
	ev->report = 1;
}

void ltntstools_tr101290_alarm_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)
{
#if LOCAL_DEBUG
	printf("%s(?, %s)\n", __func__, ltntstools_tr101290_event_name_ascii(event));
#endif
	struct tr_event_s *ev = &s->event_tbl[event];

	ev->raised = 0;
	ev->report = 1;
}

