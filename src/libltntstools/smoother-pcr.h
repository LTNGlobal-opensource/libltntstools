#ifndef _SMOOTHER_PCR_H
#define _SMOOTHER_PCR_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receiving thread doesn't own the lifespan of the buffer,
 * applications should send the output to the network inside
 * this callback.
 * Don't free the buffer when you're done with it.
 */
typedef void (*smoother_pcr_output_callback)(void *userContext, unsigned char *buf, int byteCount);

void smoother_pcr_free(void *hdl);
int  smoother_pcr_alloc(void **hdl, void *userContext, smoother_pcr_output_callback cb,
	int itemsPerSecond, int itemLengthBytes, uint64_t pcrPID, int inputMuxrate_bps);
int  smoother_pcr_write(void *hdl, const unsigned char *buf, int lengthBytes, struct timeval *ts);
//int  smoother_pcr_expire(void *hdl, struct timeval *ts);

/* From is null then from default to 1 second ago.
 *  end is null then end details to now.
 */
//int64_t smoother_pcr_sumtotal_i64(void *hdl, uint32_t channel, struct timeval *from, struct timeval *to);

#ifdef __cplusplus
};
#endif

#endif /* _SMOOTHER_PCR_H */


