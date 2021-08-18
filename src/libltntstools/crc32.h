#ifndef LTNTSTOOLS_CRC32_H
#define LTNTSTOOLS_CRC32_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ltntstools_checkCRC32(unsigned char *buf, int len);
int ltntstools_getCRC32(unsigned char *buf, int len, unsigned int *crc32);

#ifdef __cplusplus
};
#endif

#endif /* LTNTSTOOLS_CRC32_H */
