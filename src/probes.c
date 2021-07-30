#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sei-timestamp.h"
#include <libltntstools/ltntstools.h>

/* Adtec Encoder EN91 and EN100: Self identify
 *                                  A  d  t  e  c
 * pid 1fff (8191) : 47 1f ff 10 - 41 64 74 65 63 ff ff ff ff ff ff ff
 *
 * ADTEC SEI from the CNN stream:
 *                              4           8           C           F
 *  UUID              UU UU UU UU UU UU UU UU UU UU UU UU UU UU UU UU -> Data
 *  00 00 01 06 05 20 94 41 94 57 19 82 06 15 12 eb cc c1 88 01 d0 e0 ff 01 0d 01 f6 f9 09 10 40 b8 62 b8 75 c6 7f d6 80 
 *  00 00 01 06 05 20 94 41 94 57 19 82 06 15 12 eb cc c1 88 01 d0 e0 ff 01 0d 02 8c 8b 73 62 3a ca 18 ca f1 23 36 4d 80 
 *  00 00 01 06 05 20 94 41 94 57 19 82 06 15 12 eb cc c1 88 01 d0 e0 ff 01 0d 03 24 9b db 72 92 da b0 da 4c af 3a 07 80 
 *  00 00 01 06 05 20 94 41 94 57 19 82 06 15 12 eb cc c1 88 01 d0 e0 ff 01 0d 04 0b 57 f4 be bd 16 9f 16 96 eb d6 0d 80 
 *
 * VITEC two types of SEI:
 *                    UU UU UU UU UU UU UU UU UU UU UU UU UU UU UU UU -> Data
 *   VitecSEI1: 05 1c a8 68 7d d4 d7 59 37 58 a5 ce f0 33 8b 65 45 f1 1f 00 00 ff 00 00 ff d8 5f ff 72 4a 
 *   VitecSEI2: 05 1c da 84 22 1f 18 ec 53 1a 8e 05 2c 6d d1 bf 54 3a 1f 00 00 ff 03 4d ff 34 d6 ff 73 ed 
 *   VitecSEI1: 05 1c a8 68 7d d4 d7 59 37 58 a5 ce f0 33 8b 65 45 f1 1f 00 00 ff 00 00 ff d8 5f ff b3 75 
 *   VitecSEI2: 05 1c da 84 22 1f 18 ec 53 1a 8e 05 2c 6d d1 bf 54 3a 1f 00 00 ff 03 4d ff 35 d5 ff 04 80 
 *   VitecSEI1: 05 1c a8 68 7d d4 d7 59 37 58 a5 ce f0 33 8b 65 45 f1 1f 00 00 ff 00 00 ff d8 5f ff f4 a1 
 *   VitecSEI2: 05 1c da 84 22 1f 18 ec 53 1a 8e 05 2c 6d d1 bf 54 3a 1f 00 00 ff 03 4d ff 36 d3 ff 95 35 
 *   VitecSEI1: 05 1c a8 68 7d d4 d7 59 37 58 a5 ce f0 33 8b 65 45 f1 1f 00 00 ff 00 00 ff d8 60 ff 35 cc 
 *   VitecSEI2: 05 1c da 84 22 1f 18 ec 53 1a 8e 05 2c 6d d1 bf 54 3a 1f 00 00 ff 03 4d ff 37 d2 ff 25 d5 
 *
 * Ateme CM5k: Has user unregistered data but it contains a one byte value
 * Cobalt EN9992: No unregistered data
 * AVIWest HE4000: No unregistered data
 *
 */

struct ltnencoder_sei_ctx_s
{
	int64_t latencyMs;
};

int64_t ltntstools_probe_ltnencoder_get_total_latency(void *hdl)
{
	struct ltnencoder_sei_ctx_s *ctx = (struct ltnencoder_sei_ctx_s *)hdl;
	if (!ctx)
		return -1;

	return ctx->latencyMs;
}

void ltntstools_probe_ltnencoder_free(void *hdl)
{
	free(hdl);
}

int ltntstools_probe_ltnencoder_alloc(void **hdl)
{
	struct ltnencoder_sei_ctx_s *ctx = (struct ltnencoder_sei_ctx_s *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	/* Make sure we default this to -1 so if its unset, we meet the API design docs expectations. */
	ctx->latencyMs = -1;

	*hdl = ctx;
	return 0;
}

/* LTN Encoder specific function for extracting and making sense of timing information. */
static void _ltnencoder_sei_timestamp_query(struct ltnencoder_sei_ctx_s *ctx, const unsigned char *buf, int lengthBytes, int offset)
{
	struct timeval walltimeEncoderFrameEntry;
	sei_timestamp_value_timeval_query(buf + offset, lengthBytes - offset, 2, &walltimeEncoderFrameEntry);

	struct timeval walltimeLocal;
	gettimeofday(&walltimeLocal, NULL);

	/* Calculate total latency in ms from encoder frame input to this probe. */
	struct timeval diff;
	sei_timeval_subtract(&diff, &walltimeLocal, &walltimeEncoderFrameEntry);

	/* Calculate total latency in ms from encoder frame exit to this probe. */
	ctx->latencyMs = sei_timediff_to_msecs(&diff);
}

int ltntstools_probe_ltnencoder_sei_timestamp_query(void *hdl, const unsigned char *buf, int lengthBytes)
{
	struct ltnencoder_sei_ctx_s *ctx = (struct ltnencoder_sei_ctx_s *)hdl;

	/* Find the LTN Encoder UUID */
	int offset = ltn_uuid_find(buf, lengthBytes);
	if (offset >= 0) {
		_ltnencoder_sei_timestamp_query(ctx, buf, lengthBytes, offset);
		return 0; /* Success */
	}

	return -1; /* Failure */
}
