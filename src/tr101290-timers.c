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

#define LOCAL_DEBUG 0

void timer_thread_handler(union sigval sv)
{
	struct tr_event_s *ev = sv.sival_ptr;

	// Implement s, so we can
	// ltntstools_tr101290_alarm_raise(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event)

	ev->raised = 1;
	gettimeofday(&ev->lastChanged, NULL);
#if 0
	ev->nextReport = ev->lastChanged;
#endif

#if LOCAL_DEBUG
	printf("%d.%06d fired %s\n",
		(int)ev->lastRaised.tv_sec,
		(int)ev->lastRaised.tv_usec,
		ltntstools_tr101290_event_name_ascii(ev->id));
#endif
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
	sev.sigev_value.sival_ptr = ev;
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
