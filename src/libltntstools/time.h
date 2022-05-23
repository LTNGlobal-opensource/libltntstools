/* Copyright LiveTimeNet, Inc. 2020. All Rights Reserved. */

#ifndef LIBLTNTSTOOLS_TIME_H
#define LIBLTNTSTOOLS_TIME_H

#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return a YYYYMMDD-HHMMSS string in buf, for when (NULL) now, or sometime in the past */
int libltntstools_getTimestamp(char *buf, int buflen, time_t *when);
int libltntstools_getTimestamp_seperated(char *buf, int buflen, time_t *when);

#ifdef __cplusplus
};
#endif

#endif /* LIBLTNTSTOOLS_TIME_H */
