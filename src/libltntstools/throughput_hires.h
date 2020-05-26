#ifndef _THROUGHPUT_HIRES_H
#define _THROUGHPUT_HIRES_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A hiresolution scheme for tracking summable items over a usec accurate time window.
 * The caller "categorizes" values into channels, Eg. PID, or sensor id, and
 * the session can contain multiple channels for a given time period.
 */
void throughput_hires_free(void *hdl);
int  throughput_hires_alloc(void **hdl, int itemsPerSecond);
void throughput_hires_write_i64(void *hdl, uint32_t channel, int64_t value, struct timeval *ts);
int  throughput_hires_expire(void *hdl, struct timeval *ts);

/* From is null then from default to 1 second ago.
 *  end is null then end details to now.
 */
int64_t throughput_hires_sumtotal_i64(void *hdl, uint32_t channel, struct timeval *from, struct timeval *to);

#ifdef __cplusplus
};
#endif

#endif /* _THROUGHPUT_HIRES_H */


