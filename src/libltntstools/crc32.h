#ifndef LTNTSTOOLS_CRC32_H
#define LTNTSTOOLS_CRC32_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       For a given MPEG buffer, with a traditional CRC32 trailing value,
 *              validate the CRC is correct and that no corruption of the buffer has
 *              occured.
 * @param[in]   const uint8_t *buf - buffer
 * @param[in]   int lengthBytes - length in bytes
 * @return      Boolean. 1 on success else 0.
 */
int ltntstools_checkCRC32(const uint8_t *buf, int lengthBytes);

/**
 * @brief       For a given MPEG buffer, compute the correct CRC value.
 * @param[in]   const uint8_t *buf - buffer
 * @param[in]   int lengthBytes - length in bytes
 * @param[out]  int32_t *crc32 - crc value
 * @return      Boolean. 1 on success else < 0
 */
int ltntstools_getCRC32(const uint8_t *buf, int lengthBytes, uint32_t *crc32);

#ifdef __cplusplus
};
#endif

#endif /* LTNTSTOOLS_CRC32_H */
