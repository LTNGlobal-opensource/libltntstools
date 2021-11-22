#ifndef _AUDIOANALYZER_H
#define _AUDIOANALYZER_H

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

#define LTN_CODEC_ID_MP2 (0x15000 + 0) /* specifically matching avcodecs id */
#define LTN_CODEC_ID_AAC (0x15000 + 2) /* specifically matching avcodecs id */
#define LTN_CODEC_ID_AC3 (0x15000 + 3) /* specifically matching avcodecs id */

int     ltntstools_audioanalyzer_stream_add(void *hdl, uint16_t pid, uint8_t streamID, unsigned int codecID);
void    ltntstools_audioanalyzer_stream_remove(void *hdl, uint16_t pid);

int     ltntstools_audioanalyzer_alloc(void **hdl);
ssize_t ltntstools_audioanalyzer_write(void *hdl, const uint8_t *pkt, unsigned int packetCount);
void    ltntstools_audioanalyzer_free(void *hdl);

int     ltntstools_audioanalyzer_has_feature_nielsen(void *hdl);
void    ltntstools_audioanalyzer_set_verbosity(void *hdl, int level);

#ifdef __cplusplus
};
#endif

#endif /* _AUDIOANALYZER_H */
