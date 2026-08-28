/* Copyright Kernel Labs Inc, 2016 */

/* The "one-true" upstream version of the file lives in the ISO13818 project.
 * never update this file inside another project, without reflecting those
 * changes back into the upstream project also.
 */

#include "klringbuffer.h"
#include <errno.h>

#define RB_LOCK(rb) \
	if ((rb)->usingMutex) \
		pthread_mutex_lock(&(rb)->mutex);

#define RB_UNLOCK(rb) \
	if ((rb)->usingMutex) \
		pthread_mutex_unlock(&(rb)->mutex);

KLRingBuffer *rb_new(size_t size, size_t size_max)
{
	/* A single NULL return can't by itself tell a caller "you passed a bad
	 * size relationship" (a programming error) apart from "malloc() itself
	 * failed" (a runtime OOM) -- callers have historically had to guess
	 * (see pes-extractor.c's own comment on this). Set errno to a distinct,
	 * checkable value for the former; the latter already leaves errno as
	 * malloc() set it (ENOMEM).
	 */
	if ((size == 0) || (size > size_max)) {
		errno = EINVAL;
		return NULL;
	}

	KLRingBuffer *buf = (KLRingBuffer *)malloc(sizeof(*buf));
	if (!buf)
		return NULL;

	buf->data = (unsigned char *)malloc(size);
	if (!buf->data) {
		free(buf);
		return NULL;
	}

	buf->size = size;
	buf->size_initial = size;
	buf->head = buf->fill = 0;
	buf->size_max = size_max;

	pthread_mutex_init(&buf->mutex, NULL);
	buf->usingMutex = 0;

	return buf;
}

KLRingBuffer *rb_new_threadsafe(size_t size, size_t size_max)
{
	KLRingBuffer *rb = rb_new(size, size_max);
	if (rb)
		rb->usingMutex = 1;
	return rb;
}

bool rb_is_empty(KLRingBuffer *rb)
{
	if (!rb)
		return true;

	bool result = false;

	RB_LOCK(rb);
        if (rb->fill == 0)
		result = true;
	RB_UNLOCK(rb);

	return result;
}

bool rb_is_full(KLRingBuffer *rb)
{
	if (!rb)
		return false;

	bool result = false;

	RB_LOCK(rb);
	if (rb->fill == rb->size_max)
		result = true;
	RB_UNLOCK(rb);

	return result;
}

size_t _rb_used(KLRingBuffer *rb)
{
	return rb->fill;
}

size_t rb_used(KLRingBuffer *rb)
{
	if (!rb)
		return 0;

	size_t result;

	RB_LOCK(rb);
	result = _rb_used(rb);
	RB_UNLOCK(rb);

        return result;
}

size_t rb_unused(KLRingBuffer *rb)
{
	if (!rb)
		return 0;

	size_t result;

	RB_LOCK(rb);
        result = rb->size_max - rb->fill;
	RB_UNLOCK(rb);

	return result;
}

void rb_empty(KLRingBuffer *rb)
{
	if (!rb)
		return;

	RB_LOCK(rb);
        rb->head = rb->fill = 0;
	RB_UNLOCK(rb);
}

/**
 * @brief  Total number of bytes in the current allocation.
 *         Used to determine whether we need to grow the allocation upwards.
 */
static size_t _rb_size(KLRingBuffer *rb)
{
	return rb->size;
}

/**
 * @brief       The amount of free space within the current memory allocation.
 *              Used to determine whether we need to grow the allocation upwards.
 *              Humans should never need to call this.
 */
static size_t _rb_remain_in_seg(KLRingBuffer *rb)
{
	return rb->size - rb->fill;
}

static int _rb_grow(KLRingBuffer *buf, size_t increment)
{
	if (!buf)
		return -1;

	if ((_rb_size(buf) + increment) > buf->size_max) {
		return -2;
	}

	size_t new_size = buf->size + increment;
	unsigned char *new_data = (unsigned char *)malloc(new_size);
	if (!new_data)
		return -1;

	/* Linearize the existing content into the new buffer starting at
	 * offset 0, rather than realloc()-in-place: a wrapped ring's content
	 * spans two segments (head..size, then 0..remainder) in the old,
	 * smaller layout. realloc() only preserves byte offsets, not the
	 * ring's logical order, so growing a wrapped buffer in place strands
	 * the wrapped segment far from where the new (larger) modulo
	 * arithmetic expects to find it -- silently corrupting the content.
	 */
	if (buf->fill > 0) {
		size_t first = buf->size - buf->head;
		if (first > buf->fill)
			first = buf->fill;
		memcpy(new_data, buf->data + buf->head, first);
		if (buf->fill > first) {
			memcpy(new_data + first, buf->data, buf->fill - first);
		}
	}

	free(buf->data);
	buf->data = new_data;
	buf->size = new_size;
	buf->head = 0;

	return 0;
}

static void _rb_shrink_reset(KLRingBuffer *buf)
{
	/* On failure, realloc() returns NULL while leaving the original block
	 * (still valid, still buf->size bytes) untouched -- overwriting
	 * buf->data with that NULL would both leak the original allocation and
	 * hand every future write a NULL pointer to dereference. Keep the
	 * existing (larger than ideal, but valid) buffer instead; the ring is
	 * always empty here, so head/fill can still be safely reset either way,
	 * and this shrink is just an optimization that can retry next time the
	 * ring empties out.
	 */
	unsigned char *new_data = (unsigned char *)realloc(buf->data, buf->size_initial);
	if (new_data) {
		buf->data = new_data;
		buf->size = buf->size_initial;
	}
	buf->head = buf->fill = 0;
}

static inline void _advance_tail(KLRingBuffer *buf, size_t bytes)
{
	buf->fill += bytes;
}

size_t rb_get_write_pos(KLRingBuffer *buf)
{
	if (!buf)
		return 0;

	return (buf->head + buf->fill) % buf->size;
}

size_t rb_get_read_pos(KLRingBuffer *buf)
{
	if (!buf)
		return 0;

	return buf->head;
}

size_t rb_write_with_state(KLRingBuffer *buf, const char *from, size_t bytes, int *didOverflow)
{
	if (didOverflow)
		*didOverflow = 0;

	/* assert() alone (the previous guard here) compiles to nothing under
	 * NDEBUG, leaving release builds with no protection at all against a
	 * NULL buf/from -- RB_LOCK(buf) below would dereference buf immediately.
	 */
	if (!buf || !from)
		return 0;

	RB_LOCK(buf);
	if (bytes > _rb_remain_in_seg(buf)) {
		if (_rb_grow(buf, bytes * 128) < 0) {
			/* The generous bytes*128 growth hint didn't fit under
			 * size_max, but the ring may still have enough headroom left
			 * for exactly what this write needs. Retry with the minimal
			 * increment before giving up and reporting data loss that
			 * wasn't actually necessary (e.g. size=1000, size_max=1010,
			 * fill=1000, bytes=5: growing by 5*128=640 doesn't fit, but
			 * growing by the 5 actually needed does).
			 */
			size_t needed = bytes - _rb_remain_in_seg(buf);
			if (_rb_grow(buf, needed) < 0) {
				RB_UNLOCK(buf);

				/* Don't fail the write just because we've exceeded the maximum
				 * amount of storage, instead, raise an overflow and store the data anyway.
				 * Never discard more than the ring currently holds -- fill is
				 * unsigned and would otherwise wrap.
				 */
				rb_discard(buf, bytes > buf->size ? buf->size : bytes);
				if (didOverflow)
					*didOverflow = 1;

				RB_LOCK(buf);
			}
		}
	}

	/* A single write can never physically fit more than the ring's current
	 * allocation; if it's larger, only the trailing portion of it survives
	 * (matches this ring's documented "truncate on overflow" contract).
	 */
	const char *store_from = from;
	size_t store_bytes = bytes;
	if (store_bytes > buf->size) {
		store_from += (store_bytes - buf->size);
		store_bytes = buf->size;
	}

	size_t tail_offset = (buf->head + buf->fill) % buf->size;
	unsigned char *tail = buf->data + tail_offset;
	size_t to_end = buf->size - tail_offset;

	/* Decide on byte counts, not on comparing tail/write_end pointers: a
	 * write whose length is an exact multiple of buf->size wraps write_end
	 * back around to equal tail, making that comparison ambiguous between
	 * "nothing to copy" and "wraps all the way around".
	 */
	if (store_bytes <= to_end) {
		memcpy(tail, store_from, store_bytes);
	} else {
		size_t first_write = to_end;
		memcpy(tail, store_from, first_write);

		size_t second_write = store_bytes - first_write;
		memcpy(buf->data, store_from + first_write, second_write);
	}

	_advance_tail(buf, store_bytes);
	RB_UNLOCK(buf);
	return bytes;
}

size_t rb_write(KLRingBuffer *buf, const char *from, size_t bytes)
{
	return rb_write_with_state(buf, from, bytes, NULL);
}

#if 0
char *rb_write_pointer(KLRingBuffer *buf, size_t *writable)
{
    if(rb_is_full(buf))
    {
        *writable = 0;
        return NULL;
    }

    char *head = buf->data + buf->head;
    char *tail = buf->data + ((buf->head + buf->fill) % buf->size);

    if(tail < head)
    {
        *writable = head - tail;
    }
    else
    {
        char *end = buf->data + buf->size;
        *writable = end - tail;
    }

    return tail;
}

void rb_write_commit(KLRingBuffer *buf, size_t bytes)
{
    assert(bytes <= _rb_remain_in_seg(buf));
    _advance_tail(buf, bytes);
}
#endif

static inline void _advance_head(KLRingBuffer *buf, size_t bytes)
{
	buf->head = (buf->head + bytes) % buf->size;
	buf->fill -= bytes;
}

void rb_discard(KLRingBuffer *rb, size_t bytes)
{
	if (!rb)
		return;

	RB_LOCK(rb);
	/* Never discard more than is actually held -- fill is unsigned and
	 * would otherwise wrap, permanently corrupting the ring's accounting. */
	if (bytes > _rb_used(rb))
		bytes = _rb_used(rb);
	_advance_head(rb, bytes);
	RB_UNLOCK(rb);
}

static size_t rb_reader(KLRingBuffer *buf, char *to, size_t bytes, int advance_read_head)
{
	/* assert() alone (the previous guard here) compiles to nothing under
	 * NDEBUG. This backs rb_read()/rb_peek() directly (neither has its own
	 * check), so a real guard here protects both of them too.
	 */
	if (!buf || !to)
		return 0;

	if (bytes > rb_used(buf))
		bytes = rb_used(buf);

	if (bytes == 0)
		return 0;

	RB_LOCK(buf);

	unsigned char *head = buf->data + buf->head;
	unsigned char *end_read = buf->data + ((buf->head + bytes) % buf->size);

	if (end_read <= head) {
		unsigned char *end = buf->data + buf->size;

		size_t first_read = end - head;
		memcpy(to, head, first_read);

		size_t second_read = bytes - first_read;
		memcpy(to + first_read, buf->data, second_read);
	} else {
		memcpy(to, head, bytes);
	}

	if (advance_read_head)
		_advance_head(buf, bytes); 

	/* When the buffer is empty its a good time to
	 * free any prior large allocations.
	 */
	if ((_rb_used(buf) == 0) && (buf->size > buf->size_initial))
		_rb_shrink_reset(buf);

	RB_UNLOCK(buf);
	return bytes;
}

size_t rb_read(KLRingBuffer *buf, char *to, size_t bytes)
{
	return rb_reader(buf, to, bytes, 1); /* Advance read head */
}

size_t rb_read_alloc(KLRingBuffer *buf, char **to, size_t bytes)
{
	if (!buf || !to)
		return 0;

	*to = (char *)malloc(bytes);
	if (!*to)
		return 0;

	return rb_reader(buf, *to, bytes, 1); /* Advance read head */
}

size_t rb_peek(KLRingBuffer *buf, char *to, size_t bytes)
{
	return rb_reader(buf, to, bytes, 0); /* Don't Advance read head */
}

#if 0
const char *rb_read_pointer(KLRingBuffer *buf, size_t offset, size_t *readable)
{
    if(rb_is_empty(buf))
    {
        *readable = 0;
        return NULL;
    }

    char *head = buf->data + buf->head + offset;
    char *tail = buf->data + ((buf->head + offset + buf->fill) % buf->size);

    if(tail <= head)
    {
        char *end = buf->data + buf->size;
        *readable = end - head;
    }
    else
    {
        *readable = tail - head;
    }

    return head;
}

void rb_read_commit(KLRingBuffer *buf, size_t bytes)
{
    assert(rb_used(buf) >= bytes);
    _advance_head(buf, bytes);
}

void rb_stream(KLRingBuffer *from, KLRingBuffer *to, size_t bytes)
{
    assert(rb_used(from) <= bytes);
    assert(_rb_remain_in_seg(to) >= bytes);

    size_t copied = 0;
    while(copied < bytes)
    {
        size_t can_read;
        const char *from_ptr = rb_read_pointer(from, copied, &can_read);

        size_t copied_this_read = 0;
        
        while(copied_this_read < can_read)
        {
            size_t can_write;
            char *to_ptr = rb_write_pointer(to, &can_write);

            size_t write = (can_read > can_write) ? can_write : can_read;
            memcpy(to_ptr, from_ptr, write);

            copied_this_read += write;
        }

        copied += copied_this_read;
    }

    _advance_tail(to, copied);
}
#endif

void rb_free(KLRingBuffer *rb)
{
	/* RB_LOCK(rb) below dereferences rb (via rb->usingMutex) before any
	 * check could run, so the NULL guard must come first, not after --
	 * matching free()'s own "NULL is a no-op" convention that callers
	 * throughout this codebase already rely on.
	 */
	if (!rb)
		return;

	/* Lock and immediately unlock as a barrier -- wait for any in-flight
	 * critical section on this ring to finish before tearing it down. A
	 * mutex must be unlocked before pthread_mutex_destroy() (destroying a
	 * locked mutex is undefined behavior), and freeing the memory that
	 * backs it while still locked (the previous behavior here) is worse
	 * still: it can leave a lock held on storage that no longer exists.
	 *
	 * rb_new() calls pthread_mutex_init() unconditionally, regardless of
	 * whether usingMutex ever gets set -- so the matching destroy must be
	 * unconditional too, or every ring leaks its mutex's resources.
	 */
	RB_LOCK(rb);
	RB_UNLOCK(rb);
	pthread_mutex_destroy(&rb->mutex);

	free(rb->data);
	free(rb);
}

int rb_fwrite(KLRingBuffer *buf, FILE *fh)
{
	if (!buf || !fh)
		return -1;

	if (rb_is_empty(buf))
		return 0;

	/* Peek (non-destructively) the entire ring's content into a temporary
	 * buffer and only rb_discard() it once every byte has been confirmed
	 * written to fh. The previous version drained the ring via rb_read()
	 * as it went, with no fwrite() return value checked anywhere -- a
	 * write failure partway through (full disk, closed stream, ...) still
	 * destroyed the ring's data, and the void return type gave the caller
	 * no way to even know it happened.
	 */
	size_t rb_len = rb_used(buf);
	unsigned char *body = (unsigned char *)malloc(rb_len);
	if (!body)
		return -1;

	if (rb_peek(buf, (char *)body, rb_len) != rb_len) {
		free(body);
		return -1;
	}

	unsigned char head[4] = { 'H', 'E', 'A', 'D' };
	unsigned char hdrlen[4] = {
		(unsigned char)(rb_len >> 24), (unsigned char)(rb_len >> 16),
		(unsigned char)(rb_len >> 8),  (unsigned char)(rb_len)
	};
	unsigned char tail[4] = { 'T', 'A', 'I', 'L' };

	int ok = fwrite(head, 1, sizeof(head), fh) == sizeof(head) &&
		fwrite(hdrlen, 1, sizeof(hdrlen), fh) == sizeof(hdrlen) &&
		fwrite(body, 1, rb_len, fh) == rb_len &&
		fwrite(tail, 1, sizeof(tail), fh) == sizeof(tail);

	free(body);

	if (!ok)
		return -1;

	rb_discard(buf, rb_len);
	return 0;
}

