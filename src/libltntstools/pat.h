#ifndef TR101290_PAT_H
#define TR101290_PAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  ltntstools_pat_parser_alloc(void **hdl, void *userContext);
void ltntstools_pat_parser_free(void *hdl);

int ltntstools_pat_parser_write(void *hdl, const unsigned char *pkt, int packetCount, int *complete);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_PAT_H */
