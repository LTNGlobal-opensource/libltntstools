#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sei-timestamp.h"
#include <libltntstools/ltntstools.h>

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
