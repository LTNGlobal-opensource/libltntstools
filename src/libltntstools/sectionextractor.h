#ifndef _SECTIONEXTRACTOR_H
#define _SECTIONEXTRACTOR_H

/* A minimalistic implemenation of a section extractor, it has significant
 * limitations, namely:
 * 1. Only sections less than 180 bytes are supported.
 * 2. sections MUST be select contained with a single TS packet.
 * 3. TS packets must have the packet marker set, which they should anyway, for the start of a new packet.
 * Good enough for some basic lab testing with SCTE35.
 */

/* Heavily leaveraged from bits of libiso13818 */

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

int     ltntstools_sectionextractor_alloc(void **hdl, uint16_t pid, uint8_t tableID);
ssize_t ltntstools_sectionextractor_write(void *hdl, const uint8_t *pkt, size_t packetCount, int *complete);
void    ltntstools_sectionextractor_free(void *hdl);
int     ltntstools_sectionextractor_query(void *hdl, uint8_t *dst, int lengthBytes);

#endif /* _SECTIONEXTRACTOR_H */

