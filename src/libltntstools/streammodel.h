#ifndef STREAMMODEL_H
#define STREAMMODEL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libltntstools/ltntstools.h>

#ifdef __cplusplus
extern "C" {
#endif

int ltntstools_streammodel_alloc(void **hdl);
void ltntstools_streammodel_free(void *hdl);

size_t ltntstools_streammodel_write(void *hdl, const unsigned char *pkt, int packetCount, int *complete);

void ltntstools_streammodel_dprintf(void *hdl, int fd);

/* Caller is responsible for returning the allocation. */
int ltntstools_streammodel_query_model(void *hdl, struct ltntstools_pat_s **pat);

int ltntstools_streammodel_is_model_mpts(void *hdl, struct ltntstools_pat_s *pat);

#ifdef __cplusplus
};
#endif

#endif /* STREAMMODEL_H */
