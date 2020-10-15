#ifndef TR101290_TIMERS_H
#define TR101290_TIMERS_H

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

int ltntstools_tr101290_timers_create(struct ltntstools_tr101290_s *s, struct tr_event_s *ev);
int ltntstools_tr101290_timers_arm(struct ltntstools_tr101290_s *s, struct tr_event_s *ev);
int ltntstools_tr101290_timers_disarm(struct ltntstools_tr101290_s *s, struct tr_event_s *ev);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_TIMERS_H */
