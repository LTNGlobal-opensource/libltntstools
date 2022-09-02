#include "libltntstools/ts.h"
#include "libltntstools/streammodel.h"
#include <inttypes.h>

struct ltntstools_reframer_ctx_s *ltntstools_reframer_alloc(void *userContext, uint32_t frameSizeBytes, ltntstools_reframer_callback cb)
{
	if (cb == NULL)
		return NULL;

	struct ltntstools_reframer_ctx_s *ctx = calloc(1, sizeof(*ctx));
	ctx->userContext = userContext;
	ctx->minSendBytes = frameSizeBytes;
	ctx->sendBuffer = malloc(frameSizeBytes);
	ctx->cb = cb;

	return ctx;
};

void ltntstools_reframer_free(struct ltntstools_reframer_ctx_s *ctx)
{
	free(ctx->sendBuffer);
	free(ctx);
}

int ltststools_reframer_write(struct ltntstools_reframer_ctx_s *ctx, const uint8_t *buf, int lengthBytes)
{
	if (ctx == NULL)
		return -1;

	if (ctx->minSendBytes == 0) {
		ctx->cb(ctx->userContext, buf, lengthBytes);
	} else {
		int len = lengthBytes;
		int offset = 0;
		int cplen;
		while (len > 0) {
			if (len > (ctx->minSendBytes - ctx->sendIndex)) {
				cplen = ctx->minSendBytes - ctx->sendIndex;
			} else {
				cplen = len;
			}
			memcpy(ctx->sendBuffer + ctx->sendIndex, buf + offset, cplen);
			ctx->sendIndex += cplen;
			offset += cplen;
			len -= cplen;

			if (ctx->sendIndex == ctx->minSendBytes) {
				ctx->cb(ctx->userContext, ctx->sendBuffer, ctx->minSendBytes);
				ctx->sendIndex = 0;
			}
		}
	}

	return 0; /* Success */
}


