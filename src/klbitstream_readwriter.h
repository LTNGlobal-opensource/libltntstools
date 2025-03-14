/**
 * @file        klbitstream_readwriter.h
 * @author      Steven Toth <stoth@kernellabs.com>
 * @copyright	Copyright (c) 2016-2017 Kernel Labs Inc. All Rights Reserved.
 * @brief       Simplistic bitstream reader/writer capable of supporting
 *              1..64 bit writes or reads.
 *              Buffers are used exclusively in either read or write mode, and cannot be combined.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifndef KLBITSTREAM_READWRITER_H
#define KLBITSTREAM_READWRITER_H

#define KLBITSTREAM_DEBUG 1
#define KLBITSTREAM_ASSERT_ON_OVERRUN 0
#define KLBITSTREAM_RETURN_ON_OVERRUN 0
#define KLBITSTREAM_RESET_ON_OVERRUN 0
#define KLBITSTREAM_TRUNCATE_ON_OVERRUN 1
#define KTBITSTREAM_DUMP_ON_OVERRUN 0

struct klbs_context_s
{
	/* Private, so not inspect directly. Use macros where necessary. */
	uint8_t  *buf;		/* Pointer to the user allocated read/write buffer */
	uint32_t  buflen;	/* Total buffer size - Bytes */
	uint32_t  buflen_used;	/* Amount of data previously read/written to the buffer. */
	uint8_t   reg_used;	/* bits 1..8 */

	/* An 8bit shift register */
	/* Write bits are clocked in from LSB. */
	/* Read bits are clocked out from the MSB. */
	uint8_t  reg;

	int      didAllocateStorage;
	int	     overrun; // flag overrun errors
	int      truncated; // flag truncated packet length from overruns
};

/**
 * @brief       Helper Macro. Return the number of used bytes in the buffer.
 *              For a newly instantiated read buffer (or write buffer), this will always
 *              be zero. If you want to understand the absolute maximum size of the buffer, see
 *              klbs_get_buffer_size();
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @return      Return the number of used bytes in the buffer.
 */
#define klbs_get_byte_count(ctx) ((ctx)->buflen_used)

/**
 * @brief       Helper Macro. Return the buffer address.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @return      Buffer address.
 */
#define klbs_get_buffer(ctx) ((ctx)->buf)

/**
 * @brief       Helper Macro. Return the total size of the buffer, regardless
 *              of how much data has been read/written.
 *              IE: The total allocation size of the underlying memory allocation.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @return      Buffer address.
 */
#define klbs_get_buffer_size(ctx) ((ctx)->buflen)

/**
 * @brief       Helper Macro. Return the total size of the buffer, minus
 *              the number of bytes we've already read/written. Helpful when
 *              you're slowly draining a buffer and want to prevent peeking
 *              beyond the total allocate size.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @return      Buffer address.
 */
#define klbs_get_byte_count_free(ctx) (klbs_get_buffer_size(ctx) - klbs_get_byte_count(ctx))

/**
 * @brief       Allocate a new bitstream context, for read or write use.
 * @return      struct klbs_context_s *  The context itself, or NULL on error.
 */
static inline struct klbs_context_s * klbs_alloc()
{
	return (struct klbs_context_s *)calloc(1, sizeof(struct klbs_context_s));
}

/**
 * @brief       Save the entire buffer to file. This won't modify the underlying
 *              klbs_context_s in any way, nothing is lost or flushed from the buffer.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @param[in]   const char *fn  Output filename.
 * @return      0 - Success
 * @return      < 0 - Error
 */
static inline int klbs_save(struct klbs_context_s *ctx, const char *fn)
{
	FILE *fh = fopen(fn, "wb");
	if (!fh)
		return -1;

	fwrite(ctx->buf, 1, ctx->buflen_used, fh);
	fclose(fh);

	return 0;
}

/**
 * @brief       Destroy and deallocate a previously allocated context.
 *              The users read/write buffer is left in tact, and not freed.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 */
static inline void klbs_free(struct klbs_context_s *ctx)
{
	free(ctx);
}

/**
 * @brief       Initialize / reset a previously allocated context.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 */
static inline void klbs_init(struct klbs_context_s *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

/**
 * @brief       Associate a previously allocated user buffer 'buf' with the bitstream context.
 *              Subsequent calls to klbs_write_bits() will write bit-by-bit to the contexts of buf.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @param[in]   uint8_t *buf  Buffer the bistream write calls will modify
 * @param[in]   uint32_t *buf  Buffer size in bytes.
 */
static inline void klbs_write_set_buffer(struct klbs_context_s *ctx, uint8_t *buf, uint32_t lengthBytes)
{
	klbs_init(ctx);
	ctx->buf = buf;
	ctx->buflen = lengthBytes;
}

/**
 * @brief       Associate a previously allocated user buffer 'buf' with the bitstream context.
 *              Subsequent calls to klbs_read_bits() will extract bit-by-bit the contexts of buf.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @param[in]   uint8_t *buf  Buffer the bistream will read from.
 * @param[in]   uint32_t *buf  Buffer size in bytes.
 */
static inline void klbs_read_set_buffer(struct klbs_context_s *ctx, uint8_t *buf, uint32_t lengthBytes)
{
	klbs_write_set_buffer(ctx, buf, lengthBytes);
}

/**
 * @brief       Write a single bit into the bitsream buffer.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @param[in]   uint32_t bit  A single bit.
 */
static inline void klbs_write_bit(struct klbs_context_s *ctx, uint32_t bit)
{
#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (ctx->overrun)
		return;
#endif

	if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
		fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) Write Bit ctx->buflen_used %d >= ctx->buflen %d\n",
				__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
		ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
		return;
#endif
	}
	assert(ctx->buflen_used <= ctx->buflen);

	bit &= 1;
	if (ctx->reg_used < 8) {
		ctx->reg <<= 1;
		ctx->reg |= bit;
		ctx->reg_used++;
	}

	if (ctx->reg_used == 8) {
		if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
			fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) Write Bit ctx->buflen_used %d >= ctx->buflen %d\n",
					__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
			ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return;
#endif
		}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
		assert(ctx->buflen_used <= ctx->buflen);
#endif
		*(ctx->buf + ctx->buflen_used++) = ctx->reg;
		ctx->reg_used = 0;
	}
}

/**
 * @brief       Pad the bitstream buffer into byte alignment, stuff the 'bit' mutiple times to align.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @param[in]   uint32_t bit  A single bit.
 */
static inline void klbs_write_byte_stuff(struct klbs_context_s *ctx, uint32_t bit)
{
#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (ctx->overrun)
		return;
#endif

	while (ctx->reg_used > 0) {
		klbs_write_bit(ctx, bit);
		if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
			fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) Write Byte Stuff ctx->buflen_used %d >= ctx->buflen %d\n",
					__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
			ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return;
#endif
		}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
		assert(ctx->buflen_used <= ctx->buflen);
#endif
	}
}

/**
 * @brief       Write multiple bits of data into the previously associated user buffer.
 *              Writes are LSB justified, so the bits value 0x101, is nine bits.
 *              Omitting this step could lead to a bistream thats one byte too short.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @param[in]   uint32_t bits  data pattern.
 * @param[in]   uint32_t bitcount  number of bits to write
 */
static inline void klbs_write_bits(struct klbs_context_s *ctx, uint64_t bits, uint32_t bitcount)
{
#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (ctx->overrun)
		return;
#endif

	for (int i = (bitcount - 1); i >= 0; i--) {
		klbs_write_bit(ctx, bits >> i);
		if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
			fprintf(stderr, "KLBITSTREAM OVERRUN: (%s:%s:%d) Write Bits ctx->buflen_used %d >= ctx->buflen %d\n",
					__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
			ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return;
#endif
		}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
		assert(ctx->buflen_used <= ctx->buflen);
#endif
	}
}

/**
 * @brief       Flush any intermediate bits out to the buffer.
 *              Callers typically do this when no more data needs to be written and the bitstream
 *              is considered complete. This ensures that any dangling trailing bits are properly
 *              stuffer and written to the buffer.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 */
static inline void klbs_write_buffer_complete(struct klbs_context_s *ctx)
{
#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (ctx->overrun)
		return;
#endif

	if (ctx->reg_used > 0) {
		for (int i = ctx->reg_used; i <= 8; i++) {
			klbs_write_bit(ctx, 0);
			if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
				fprintf(stderr, "KLBITSTREAM OVERRUN: Write Buffer Complete (%s:%s:%d) ctx->buflen_used %d >= ctx->buflen %d\n",
						__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
				ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
				return;
#endif
			}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
			assert(ctx->buflen_used <= ctx->buflen);
#endif
		}
	}
}

/**
 * @brief       Read a single bit from the bitstream.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @return      uint32_t  a bit
 */
static inline uint32_t klbs_read_bit(struct klbs_context_s *ctx)
{
	uint32_t bit = 0;

#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (ctx->overrun)
		return bit;
#endif

	if (!(ctx->buflen_used <= ctx->buflen)) {
#if KLBITSTREAM_DEBUG
		printf("KLBITSTREAM OVERRUN: (%s:%s:%d) Read Bit ctx->buflen_used %d > ctx->buflen %d\n",
				__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
		ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
		return 0;
#endif
	}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
	assert(ctx->buflen_used <= ctx->buflen);
#endif

	if (ctx->reg_used == 0) {
		if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
			printf("KLBITSTREAM OVERRUN: (%s:%s:%d) Read Bit ctx->buflen_used %d >= ctx->buflen %d\n",
					__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
			ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return 0;
#endif
		}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
		assert(ctx->buflen_used <= ctx->buflen);
#endif
		ctx->reg = *(ctx->buf + ctx->buflen_used++);
		ctx->reg_used = 8;
	}

	if (ctx->reg_used <= 8) {
		bit = ctx->reg & 0x80 ? 1 : 0;
		ctx->reg <<= 1;
		ctx->reg_used--;
	}
	return bit;
}

static uint64_t klbs_read_byte_aligned(struct klbs_context_s *ctx)
{
	if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
		printf("KLBITSTREAM OVERRUN: (%s:%s:%d) Read Byte Aligned ctx->buflen_used %d >= ctx->buflen %d\n",
				__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
		ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
		return 0;
#endif
	}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
	assert(ctx->buflen_used <= ctx->buflen);
#endif
	return *(ctx->buf + ctx->buflen_used++);
}

/**
 * @brief       Read between 1..64 bits from the bitstream.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @return      uint64_t  bits
 */
static inline uint64_t klbs_read_bits(struct klbs_context_s *ctx, uint32_t bitcount)
{
	uint64_t bits = 0;

#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (ctx->overrun)
		return bits;
#endif

	if (bitcount == 8 && ctx->reg_used == 0)
		return klbs_read_byte_aligned(ctx);

	for (uint32_t i = 1; i <= bitcount; i++) {
		bits <<= 1;
		bits |= klbs_read_bit(ctx);
		if (ctx->overrun) {
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return bits;
#endif
		}
		if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
			printf("KLBITSTREAM OVERRUN: (%s:%s:%d) Read Bits ctx->buflen_used %d >= ctx->buflen %d\n",
					__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
			ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return bits;
#endif
		}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
		assert(ctx->buflen_used <= ctx->buflen);
#endif
	}
	return bits;
}

/**
 * @brief       Peek between 1..64 bits from the bitstream.
 *              Each call to peek copies the context, advances it, without changing the
 *              original context. As a result, consecutive peek calls will always return
 *              the same content.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 * @return      uint64_t  bits
 */
static inline uint64_t klbs_peek_bits(struct klbs_context_s *ctx, uint32_t bitcount)
{
	if (ctx->buflen_used + bitcount >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
		printf("KLBITSTREAM OVERRUN: (%s:%s:%d) Peek Bits ctx->buflen_used %d + bitcount %d >= ctx->buflen %d\n",
				__FILE__, __func__, __LINE__, ctx->buflen_used, bitcount, ctx->buflen);
#endif
		ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
		return 0;
#endif
	}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
	assert(ctx->buflen_used + bitcount <= ctx->buflen);
#endif
	struct klbs_context_s copy = *ctx; /* Implicit struct copy */
	return klbs_read_bits(&copy, bitcount);
}

/**
 * @brief       Read and discard all bits in the buffer until we're byte aligned again.\n
 *              The sister function to klbs_write_byte_stuff();
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 */
static inline void klbs_read_byte_stuff(struct klbs_context_s *ctx)
{
#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (ctx->overrun)
		return;
#endif

	while (ctx->reg_used > 0) {
		klbs_read_bit(ctx);
		if (ctx->buflen_used >= ctx->buflen) {
#if KLBITSTREAM_DEBUG
			printf("KLBITSTREAM OVERRUN: (%s:%s:%d) Read Byte Stuff ctx->buflen_used %d >= ctx->buflen %d\n",
					__FILE__, __func__, __LINE__, ctx->buflen_used, ctx->buflen);
#endif
			ctx->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return;
#endif
		}
#if KLBITSTREAM_ASSERT_ON_OVERRUN
		assert(ctx->buflen_used <= ctx->buflen);
#endif
	}
}

/**
 * @brief       Peek between 1..N bits from the bitstream, dump to console in binary format.
 *              Each call to peek copies the context, advances it, without changing the
 *              original context. As a result, consecutive peek calls will always return
 *              the same content.
 * @param[in]   struct klbs_context_s *ctx  bitstream context
 */
static inline void klbs_peek_print_binary(struct klbs_context_s *ctx, uint32_t bitcount)
{
	const char *space = " ";
	const char *nospace = "";
	struct klbs_context_s copy = *ctx; /* Implicit struct copy */
	for (uint32_t i = 1; i <= bitcount && (copy.buflen_used <= copy.buflen); i++) {
		printf("%d%s", klbs_read_bit(&copy), (i % 8 == 0) ? space : nospace);
	}
	printf("\n");
}

/**
 * @brief       Allocate a new bitstream context, for read or write use.
 * @param[in]   uint32_t storageSizeBytes - Buffer size to allocate
 * @param[in]   int writeMode - indicate 1 for write mode buffer, or 0 for read mode.
 * @return      struct klbs_context_s *  The context itself, or NULL on error.
 */
static inline struct klbs_context_s * klbs_alloc_init_with_storage(uint32_t storageSizeBytes, int writeMode)
{
	struct klbs_context_s *ctx = calloc(1, sizeof(struct klbs_context_s));
	if (!ctx)
		return NULL;

	klbs_init(ctx);
	ctx->didAllocateStorage = 1;

	uint8_t *buf = calloc(1, storageSizeBytes);
	if (!buf) {
		free(ctx);
		return NULL;
	}

	if (writeMode)
		klbs_write_set_buffer(ctx, buf, storageSizeBytes);
	else
		klbs_read_set_buffer(ctx, buf, storageSizeBytes);

	return ctx;
}

/**
* @brief       Move 'bits' bits from src and write to dst. Source will be formally de-serialized.
* @param[in]   struct klbs_context_s *dst - destination
* @param[in]   struct klbs_context_s *src - souce
* @param[in]   size_t bits - number of bits to copy
* @return      struct klbs_context_s *  The context itself, or NULL on error.
*/
static inline void klbs_bitmove(struct klbs_context_s *dst, struct klbs_context_s *src, size_t bits)
{
#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (src->overrun)
		return;
#endif

	for(int i = 0; i < bits; i++) {
		klbs_write_bit(dst, klbs_read_bit(src));
		if (src->overrun || dst->overrun) {
#if KLBITSTREAM_DEBUG
			fprintf(stderr, "KLBITSTREAM OVERRUN: Bitmove (%s:%s:%d) src->overrun %d dst->overrun %d\n", __FILE__, __func__, __LINE__, src->overrun, dst->overrun);
#endif
#if KLBITSTREAM_RETURN_ON_OVERRUN
			return;
#endif
#if KLBITSTREAM_ASSERT_ON_OVERRUN
			assert(src->overrun == 0 && dst->overrun == 0);
#endif
		}
	}
}

/**
* @brief       Copy 'bits' bits from src and write to dst. The state of src is not adjusted or modified.
* @param[in]   struct klbs_context_s *dst - destination
* @param[in]   struct klbs_context_s *src - souce
* @param[in]   size_t bits - number of bits to copy
* @return      struct klbs_context_s *  The context itself, or NULL on error.
*/
static inline void klbs_bitcopy(struct klbs_context_s *dst, struct klbs_context_s *src, size_t bits)
{
	struct klbs_context_s copy = *src; /* Implicit struct copy */
#if KLBITSTREAM_RETURN_ON_OVERRUN
	if (src->overrun)
		return;
#endif
	if (src->buflen_used + bits >= src->buflen || dst->buflen_used + bits >= dst->buflen) {
#if KLBITSTREAM_DEBUG
		fprintf(stderr, "KLBITSTREAM OVERRUN: Bitcopy (%s:%s:%d) src->buflen_used %d + bits %d >= src->buflen %d\n",
				__FILE__, __func__, __LINE__, src->buflen_used, (int)bits, src->buflen);
#endif
		src->overrun = 1;
		dst->overrun = 1;
#if KLBITSTREAM_RETURN_ON_OVERRUN
		return;
#endif
	}

	return klbs_bitmove(dst, &copy, bits);
}

#endif /* KLBITSTREAM_READWRITER_H */
