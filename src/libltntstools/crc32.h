#ifndef LTNTSTOOLS_CRC32_H
#define LTNTSTOOLS_CRC32_H

/**
 * @file        crc32.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2017-2022 Kernel Labs Inc. All Rights Reserved.
 * @brief       CRC MPEG generation and validation
 */

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
 *              occured. The CRC32 is expected to be the last 4 bytes of buf, MSB
 *              first, per the standard ISO/IEC 13818-1 section CRC_32 field.
 * @param[in]   const uint8_t *buf - buffer, including its trailing 4 byte CRC32.
 * @param[in]   int lengthBytes - length of buf in bytes, including the trailing
 *              CRC32. Must be >= 4.
 * @return      0 on success (CRC is valid), else < 0 on error or CRC mismatch
 *              (eg. buf is NULL or lengthBytes < 4).
 */
int ltntstools_checkCRC32(const uint8_t *buf, int lengthBytes);

/**
 * @brief       For a given MPEG buffer, compute the correct CRC value.
 *              Does not append the CRC to buf, the caller owns any modification of buf.
 * @param[in]   const uint8_t *buf - buffer, not including any trailing CRC32.
 * @param[in]   int lengthBytes - length of buf in bytes. Must be >= 1.
 * @param[out]  uint32_t *crc32 - On success, receives the computed CRC value.
 * @return      0 on success, else < 0 on error (eg. buf or crc32 is NULL, or
 *              lengthBytes < 1). *crc32 is unmodified on error.
 */
int ltntstools_getCRC32(const uint8_t *buf, int lengthBytes, uint32_t *crc32);

#ifdef __cplusplus
};
#endif

#endif /* LTNTSTOOLS_CRC32_H */
