/* Copyright Kernel Labs Inc, 2016 */

/* The "one-true" upstream version of the file lives in the ISO13818 project.
 * never update this file inside another project, without reflecting those
 * changes back into the upstream project also.
 */

#include "klringbuffer.h"

#define RB_LOCK(rb) \
	if ((rb)->usingMutex) \
		pthread_mutex_lock(&(rb)->mutex);

#define RB_UNLOCK(rb) \
	if ((rb)->usingMutex) \
		pthread_mutex_unlock(&(rb)->mutex);

KLRingBuffer *rb_new(size_t size, size_t size_max)
{
	if ((size == 0) || (size > size_max))
		return 0;

	KLRingBuffer *buf = (KLRingBuffer *)malloc(sizeof(*buf));
	if (!buf)
		return 0;

	buf->data = (unsigned char *)malloc(size);
	if (!buf->data) {
		free(buf);
		return 0;
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
	bool result = false;

	RB_LOCK(rb);
        if (rb->fill == 0)
		result = true;
	RB_UNLOCK(rb);

	return result;
}

bool rb_is_full(KLRingBuffer *rb)
{
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
	size_t result;

	RB_LOCK(rb);
	result = _rb_used(rb);
	RB_UNLOCK(rb);

        return result;
}

size_t rb_unused(KLRingBuffer *rb)
{
	size_t result;

	RB_LOCK(rb);
        result = rb->size_max - rb->fill;
	RB_UNLOCK(rb);

	return result;
}

void rb_empty(KLRingBuffer *rb)
{
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
	buf->data = (unsigned char *)realloc(buf->data, buf->size_initial);
	buf->size = buf->size_initial;
	buf->head = buf->fill = 0;
}

static inline void _advance_tail(KLRingBuffer *buf, size_t bytes)
{
	buf->fill += bytes;
}

unsigned int rb_get_write_pos(KLRingBuffer *buf)
{
	return (buf->head + buf->fill) % buf->size;
}

unsigned int rb_get_read_pos(KLRingBuffer *buf)
{
	return buf->head;
}

size_t rb_write_with_state(KLRingBuffer *buf, const char *from, size_t bytes, int *didOverflow)
{
	assert(buf);
	assert(from);

	if (didOverflow)
		*didOverflow = 0;
	RB_LOCK(buf);
	if (bytes > _rb_remain_in_seg(buf)) {
		if (_rb_grow(buf, bytes * 128) < 0) {
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
	assert(buf);
	assert(to);

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

	RB_LOCK(rb);

	free(rb->data);
	free(rb);
}

void rb_fwrite(KLRingBuffer *buf, FILE *fh)
{
	if (rb_is_empty(buf))
		return;

	unsigned char head[4] = { 'H', 'E', 'A', 'D' };
	fwrite(&head[0], 1, sizeof(head), fh);

	unsigned int rb_len = rb_used(buf);
	unsigned char hdrlen[4];
	hdrlen[0] = rb_len >> 24;
	hdrlen[1] = rb_len >> 16;
	hdrlen[2] = rb_len >>  8;
	hdrlen[3] = rb_len;

	fwrite(&hdrlen[0], 1, sizeof(hdrlen), fh);

	unsigned char b[8192];
	size_t len = 1;
	while (len) {
		len = rb_read(buf, (char *)&b[0], sizeof(b));
		if (len)
			fwrite(&b[0], 1, len, fh);
	}

	unsigned char tail[4] = { 'T', 'A', 'I', 'L' };
	fwrite(&tail[0], 1, sizeof(tail), fh);
}

