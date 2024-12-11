#ifndef TR101290_EVENTS_H
#define TR101290_EVENTS_H

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

struct tr_event_s *ltntstools_tr101290_event_table_copy();

int ltntstools_tr101290_event_should_report(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event, struct timeval *now);

void _tr101290_event_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event);

int _event_table_entry_count(struct ltntstools_tr101290_s *s);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_EVENTS_H */
