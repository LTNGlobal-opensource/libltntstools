#ifndef REFRAMER_H
#define REFRAMER_H

/**
 * @file        reframer.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       Pack streamers of packets, typeing 3-4 packets long into a fixed N packet length (7)./
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief       Callback function definition, where frames chunks of data are emiited.
 *              For example 7 * 188 bytes could be emmited.
 *              The receiver doesn't own the object lifespan, copy the allocation if you need it.
 */
typedef void (*ltntstools_reframer_callback)(void *userContext, const uint8_t *buf, int lengthBytes);

/**
 * @brief       Reframer object context, don't directly inspect or modify these.
 */
struct ltntstools_reframer_ctx_s
{
	ltntstools_reframer_callback  cb;
	void                         *userContext;
	uint8_t                      *sendBuffer;
	int                           sendIndex;
	int                           minSendBytes;
};

/**
 * @brief       Allocate a framework context capable of chunking streams of bytes into framed size of frameSizeBytes.
 *              Call the callback when a chunk is ready for processing. We use this to re-frame non 7*188 udp frames
 *              into something broadcast equipment likes (7 * 188) frames.
 * @param[in]   void *userContext - Returned to your applicaiton during callback.
 * @param[in]   uint32_t frameSizeBytes - Output frame size
 * @param[in]   ltntstools_reframer_callback cb - User defined callback.
 * @return      ptr on success else NULL.
 */
struct ltntstools_reframer_ctx_s *ltntstools_reframer_alloc(void *userContext, uint32_t frameSizeBytes, ltntstools_reframer_callback cb);

/**
 * @brief       Allocate a framework context capable of chunking streams of bytes into framed size of frameSizeBytes.
 *              Call the callback when a chunk is ready for processing. We use this to re-frame non 7*188 udp frames
 *              into something broadcast equipment likes (7 * 188) frames.
 * @param[in]   struct ltntstools_reframer_ctx_s *ctx - object.
 * @param[in]   const uint8_t *buf - buffer of bytes
 * @param[in]   int lengthBytes - buffer length
 * @return      0 on success, else < 0.
 */
int ltststools_reframer_write(struct ltntstools_reframer_ctx_s *ctx, const uint8_t *buf, int lengthBytes);

/**
 * @brief       Free a previously allocated framework context.
 * @param[in]   struct ltntstools_reframer_ctx_s *ctx - object.
 */
void ltntstools_reframer_free(struct ltntstools_reframer_ctx_s *ctx);

#endif /* REFRAMER_H */

