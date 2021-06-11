#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/statvfs.h>

#include "libltntstools/segmentwriter.h"
#include "libltntstools/time.h"
#include "libltntstools/kl-queue.h"
#include "klringbuffer.h"

#define LOCAL_DEBUG 0
#define LOG_FILE 0
#define USE_QUEUE_NOT_RING 1

#if USE_QUEUE_NOT_RING
struct q_item_s
{
	unsigned char *ptr;
	int lengthBytes;
};

struct q_item_s *q_item_malloc(const unsigned char *buf, int lengthBytes)
{
	struct q_item_s *i = malloc(sizeof(*i));
	if (!i)
		return NULL;

	i->ptr = malloc(lengthBytes);
	if (!i->ptr) {
		free(i);
		return NULL;
	}
	i->lengthBytes = lengthBytes;

	if (buf) {
		memcpy(i->ptr, buf, i->lengthBytes);
	}

	return i;
};

void q_item_free(struct q_item_s *i)
{
	if (i->ptr) {
		free(i->ptr);
		i->lengthBytes = 0;
	}
	free(i);
}

#endif

struct ltntstools_segmentwriter_s
{
	time_t lastOpen;
	pthread_t threadId;

	pthread_mutex_t mutex;
#if USE_QUEUE_NOT_RING
	struct klqueue_s q;
#else
	KLRingBuffer *rb;
#endif

	/* 0 = Write a single large file, 1 = write 1min segments. */
	int writeMode;

	int threadTerminate, threadTerminated, threadRunning;

	time_t recordingStartTime;

	int64_t totalBytesWritten; /* Across the entire recording including all segments */

	int totalSegmentsCreated;
	FILE *fh;
	char *filenamePrefix;
	char *filenameSuffix;
	char *filename;

	unsigned char *fileHeader;
	int fileHeaderLength;
};

static void swlog(struct ltntstools_segmentwriter_s *s, const char *msg)
{
#if LOG_FILE
	char ts[64];
	libltntstools_getTimestamp(&ts[0], sizeof(ts), NULL);

	char fn[64];
	sprintf(fn, "/tmp/segwriter-%d.log", getpid());
	FILE *fh = fopen(fn, "a+");
	if (fh) {
		fprintf(fh, "%s: %s", ts, msg);
		fclose(fh);
	}
#endif
}

static size_t _write(struct ltntstools_segmentwriter_s *s)
{
	time_t now = time(NULL);

	if (s->writeMode == 1 && (s->fh)) {
		if (now >= (s->lastOpen + 60)) {
			swlog(s, "Closing\n");
			fclose(s->fh);
			s->fh = NULL;
		}
	}

	if (s->writeMode == 0) {
		if (s->fh == NULL) {
			if (s->filename) {
				free(s->filename);
				s->filename = NULL;
			}
			char ts[64];
			libltntstools_getTimestamp(&ts[0], sizeof(ts), NULL);
			s->filename = realloc(s->filename, 512);
			sprintf(s->filename, "%s-%s%s", s->filenamePrefix, ts, s->filenameSuffix);
		}
	} else
	if (s->writeMode == 1) {
		if (s->fh == NULL) {
			char ts[64];
			libltntstools_getTimestamp(&ts[0], sizeof(ts), NULL);
			if (s->filename) {
				free(s->filename);
				s->filename = NULL;
			}
			s->filename = realloc(s->filename, 512);
			sprintf(s->filename, "%s-%s%s", s->filenamePrefix, ts, s->filenameSuffix);
		}
	}

	if (s->fh == NULL) {
		char msg[256];
		sprintf(msg, "Opening %s\n", s->filename);
		swlog(s, msg);
#if LOCAL_DEBUG
		printf(msg);
#endif

		s->fh = fopen(s->filename, "wb");
		s->lastOpen = time(NULL);
		if (s->recordingStartTime == 0)
			s->recordingStartTime = s->lastOpen;
		s->totalSegmentsCreated++;
		if (s->fh) {
			fwrite(s->fileHeader, 1, s->fileHeaderLength, s->fh);
			s->totalBytesWritten += s->fileHeaderLength;
		}
	}

	if (s->fh) {
		pthread_mutex_lock(&s->mutex);
#if USE_QUEUE_NOT_RING
		while (!klqueue_empty(&s->q)) {
			struct q_item_s *qi = NULL;
			int ret = klqueue_pop_non_blocking(&s->q, 2000, (void **)&qi);
			if (ret == 0) {
				fwrite(qi->ptr, 1, qi->lengthBytes, s->fh);
				s->totalBytesWritten += qi->lengthBytes;
				q_item_free(qi);
			} else {
				break;
			}
		}
#else
		int len = rb_used(s->rb);
		if (len > 0) {
			char *buf = malloc(len);
			size_t rlen = rb_read(s->rb, buf, len);

			{
				char msg[256];
				sprintf(msg, "Requested ring %d bytes got %d\n", len, (int)rlen);
				swlog(s, msg);
			}

			if (rlen != len)
			{
				char msg[256];
				sprintf(msg, "Requested ring %d bytes got %d, error\n", len, (int)rlen);
				swlog(s, msg);
			}

			fwrite(buf, 1, len, s->fh);
			s->totalBytesWritten += len;
			free(buf);
		}
#endif
		pthread_mutex_unlock(&s->mutex);
	}

	return 0;
}

void *ltntstools_segmentwriter_threadFunc(void *p)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)p;

	s->threadRunning = 1;

	while (!s->threadTerminate) {
		usleep(10 * 1000); /* TODO: Replace this with a semaphore */
#if USE_QUEUE_NOT_RING
		if (!klqueue_empty(&s->q)) {
#else
		if (!rb_is_empty(s->rb)) {
#endif
			_write(s);
		}
	}

	free(s->filenamePrefix);
	free(s);

	s->threadRunning = 0;
	s->threadTerminated = 1;
	return NULL;
}

int ltntstools_segmentwriter_alloc(void **hdl, const char *filenamePrefix, const char *filenameSuffix, int writeMode)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)calloc(1, sizeof(*s));

	pthread_mutex_init(&s->mutex, NULL);
	s->filenamePrefix = strdup(filenamePrefix);
	if (filenameSuffix == NULL)
		s->filenameSuffix = strdup(".ts");
	else
		s->filenameSuffix = strdup(filenameSuffix);
	s->writeMode = writeMode;
#if USE_QUEUE_NOT_RING
	klqueue_initialize(&s->q);
#else
	s->rb = rb_new(4 * 1048576, 16 * 1048576);
#endif

	*hdl = s;

	return pthread_create(&s->threadId, NULL, ltntstools_segmentwriter_threadFunc, s);
}

void ltntstools_segmentwriter_free(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (s->threadRunning) {
		s->threadTerminate = 1;
		while (!s->threadTerminated) {
			usleep(10 * 1000);
		}
	}
	if (s->fileHeader) {
		free(s->fileHeader);
		s->fileHeader = NULL;
		s->fileHeaderLength = 0;
	}
#if USE_QUEUE_NOT_RING
	klqueue_destroy(&s->q);
#else
	if (s->rb) {
		rb_free(s->rb);
		s->rb = NULL;
	}
#endif
}

/* alloc a buffer of length bytes, caller will fill dst and return it to us later via
 * ltntstools_segmentwriter_object_write(hdl, obj)
 */
int ltntstools_segmentwriter_object_alloc(void *hdl, size_t length, void **obj, uint8_t **dst)
{
#if USE_QUEUE_NOT_RING
	struct q_item_s *qi = q_item_malloc(NULL, length);
	if (!qi) {
		return -1;
	}
	*obj = qi;
	*dst = qi->ptr;

	return 0;
#else
	return -1;
#endif
}

int ltntstools_segmentwriter_object_write(void *hdl, void *object)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;

	struct q_item_s *qi = (struct q_item_s *)object;
	klqueue_push(&s->q, qi);

	return qi->lengthBytes;

#if USE_QUEUE_NOT_RING
#else
	return -1;
#endif
}

ssize_t ltntstools_segmentwriter_write(void *hdl, const uint8_t *buf, size_t length)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;

#if USE_QUEUE_NOT_RING
	ssize_t len = 0;
	struct q_item_s *qi = q_item_malloc(buf, length);
	if (!qi) {
		return 0;
	}
	klqueue_push(&s->q, qi);
#else
	pthread_mutex_lock(&s->mutex);
	int didOverflow;
	ssize_t len = rb_write_with_state(s->rb, (const char *)buf, length, &didOverflow);
	pthread_mutex_unlock(&s->mutex);
#endif

#if USE_QUEUE_NOT_RING
#else
	if (didOverflow) {
#if 0
		fprintf(stderr, "%s() didOverflow\n", __func__);
#else
		char msg[256];
		sprintf(msg, "ringbuffer overflow, wlen %d, used = %d unused = %d\n",
			(int)length, (int)rb_used(s->rb), (int)rb_unused(s->rb));
		swlog(s, msg);
#endif
	}
#endif

	return len;
}

int ltntstools_segmentwriter_get_current_filename(void *hdl, char *dst, int lengthBytes)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (!s->fh)
		return -1;

	strncpy(dst, &s->filename[0], lengthBytes);

	return 0;
}

int ltntstools_segmentwriter_set_header(void *hdl, const uint8_t *buf, size_t length)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;

	if (s->fileHeader) {
		free(s->fileHeader);
		s->fileHeader = NULL;
		s->fileHeaderLength = 0;
	}

	s->fileHeader = malloc(length);
	if (!s->fileHeader)
		return -1;

	s->fileHeaderLength = length;
	memcpy(s->fileHeader, buf, s->fileHeaderLength);

	return 0;
}

double ltntstools_segmentwriter_get_freespace_pct(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (!s->fh)
		return -1;

	struct statvfs fs;
	int ret = statvfs(&s->filename[0], &fs);
	if (ret != 0)
		return ret;

	double v = (double)fs.f_bfree / (double)fs.f_blocks;
	v *= 100.0;

	return v;
}

int ltntstools_segmentwriter_get_segment_count(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (!s->fh)
		return -1;

	return s->totalSegmentsCreated;
}

int64_t ltntstools_segmentwriter_get_recording_size(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (!s->fh)
		return -1;

	return s->totalBytesWritten;
}

time_t ltntstools_segmentwriter_get_recording_start_time(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (!s->fh)
		return -1;

	return s->recordingStartTime;
}

int ltntstools_segmentwriter_get_queue_depth(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	return klqueue_count(&s->q);
}

