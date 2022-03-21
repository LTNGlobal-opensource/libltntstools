#ifndef TR101290_ALARMS_H
#define TR101290_ALARMS_H

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

/* These calls set and clear bits in the ctx->event_tbl array.
 * Those bits are then acted upon by a background thread.
 */
void ltntstools_tr101290_alarm_raise(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event);
void ltntstools_tr101290_alarm_clear(struct ltntstools_tr101290_s *s, enum ltntstools_tr101290_event_e event);

void ltntstools_tr101290_alarm_raise_all(struct ltntstools_tr101290_s *s);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_ALARMS_H */
