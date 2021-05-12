#ifndef _SEGMENTWRITER_H
#define _SEGMENTWRITER_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

int     ltntstools_segmentwriter_alloc(void **hdl, const char *filenamePrefix, const char *filenameSuffix, int writeMode);
int     ltntstools_segmentwriter_set_header(void *hdl, const uint8_t *buf, size_t length);
ssize_t ltntstools_segmentwriter_write(void *hdl, const uint8_t *buf, size_t length);
void    ltntstools_segmentwriter_free(void *hdl);
int     ltntstools_segmentwriter_get_current_filename(void *hdl, char *dst, int lengthBytes);

#ifdef __cplusplus
};
#endif

#endif /* _SEGMENTWRITER_H */


