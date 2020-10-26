#ifndef STREAMMODEL_H
#define STREAMMODEL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ltntstools_streammodel_alloc(void **hdl);
void ltntstools_streammodel_free(void *hdl);

size_t ltntstools_streammodel_write(void *hdl, const unsigned char *pkt, int packetCount, int *complete);

void ltntstools_streammodel_dprintf(void *hdl, int fd);

#ifdef __cplusplus
};
#endif

#endif /* STREAMMODEL_H */
