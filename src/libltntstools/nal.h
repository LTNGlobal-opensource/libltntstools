#ifndef NAL_H
#define NAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
};
#endif

/**
 * @brief         Search buffer for the byte sequence 000001, a NAL header signature.
 * @param[in]     const uint8_t *buf - Buffer of data, possibly containing none or more NAL packets.
 * @param[in]     int lengthBytes - Buffer length in bytes.
 * @param[in/out] int offset - Enumerator. Caller MUST initalize to -1 before first call.
 *                             Function will use the contents off offset to enumerate the
 *                             entire buffer over multiple calls.
 * @return          0 - Success
 * @return        < 0 - Error
 */
int ltn_nal_findHeader(const uint8_t *buf, int lengthBytes, int *offset);

char *ltn_nal_hevc_findNalTypes(const uint8_t *buf, int lengthBytes);

#endif /* NAL_H */
