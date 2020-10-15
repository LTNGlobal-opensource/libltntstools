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

struct timer_context_s
{
	struct ltntstools_tr101290_s *s;
	struct tr_event_s *ev;
};
struct timer_context_s *_new_timer_context(struct ltntstools_tr101290_s *s, struct tr_event_s *ev)
{
	struct timer_context_s *ctx = malloc(sizeof(*ctx));
	ctx->s = s;
	ctx->ev = ev;
	return ctx;
}

void timer_thread_handler(union sigval sv)
{
	struct timer_context_s *ctx = sv.sival_ptr;
	struct ltntstools_tr101290_s *s = ctx->s;
	struct tr_event_s *ev = ctx->ev;

#if LOCAL_DEBUG
	printf("%d.%06d fired %s\n",
		(int)ev->lastChanged.tv_sec,
		(int)ev->lastChanged.tv_usec,
		ltntstools_tr101290_event_name_ascii(ev->id));
#endif
	ltntstools_tr101290_alarm_raise(s, ev->id);
}

int ltntstools_tr101290_timers_create(struct ltntstools_tr101290_s *s, struct tr_event_s *ev)
{
#if LOCAL_DEBUG
	printf("%s() %s\n", __func__, ltntstools_tr101290_event_name_ascii(ev->id));
#endif
	struct sigevent sev;
	memset(&sev, 0, sizeof(sev));

	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = &timer_thread_handler;
	sev.sigev_value.sival_ptr = _new_timer_context(s, ev);
	int ret = timer_create(CLOCK_REALTIME, &sev, &ev->timerId);
	if (ret != 0) {
		fprintf(stderr, "%s() error creating timer.\n", __func__);
		return -1;
	}

	return 0;
}

int ltntstools_tr101290_timers_arm(struct ltntstools_tr101290_s *s, struct tr_event_s *ev)
{
#if LOCAL_DEBUG
	printf("%s() %s\n", __func__, ltntstools_tr101290_event_name_ascii(ev->id));
#endif
	/* Setup a timer to begin 1 second from now, to expire in N milliseconds, as determined by the overall event. */
	struct itimerspec t;
	t.it_value.tv_sec = 1;
	t.it_value.tv_nsec = 0;
	t.it_interval.tv_sec = 0;
	t.it_interval.tv_nsec = ev->timerAlarmPeriodms * 1000000;
	int ret = timer_settime(ev->timerId, 0, &t, NULL);
	if (ret != 0)
		return -1;

	return 0;
}

int ltntstools_tr101290_timers_disarm(struct ltntstools_tr101290_s *s, struct tr_event_s *ev)
{
	struct itimerspec t;
	t.it_value.tv_sec = 0;
	t.it_value.tv_nsec = 0;
	t.it_interval.tv_sec = 0;
	t.it_interval.tv_nsec = 0;
	int ret = timer_settime(ev->timerId, 0, &t, NULL);
	if (ret != 0)
		return -1;

	return 0;
}
