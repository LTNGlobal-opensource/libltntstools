#ifndef TR101290_P2_H
#define TR101290_P2_H

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
#include "libltntstools/streammodel.h"

ssize_t p2_write(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount, struct timeval *time_now);

void *p2_streammodel_callback(void *userContext, struct streammodel_callback_args_s *args);

void p2_process_p2_2(struct ltntstools_tr101290_s *s);
void p2_process_pat_model(struct ltntstools_tr101290_s *s, struct ltntstools_pat_s *pat);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_P2_H */
