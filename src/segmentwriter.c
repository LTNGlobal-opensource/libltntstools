#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "libltntstools/segmentwriter.h"
#include "libltntstools/time.h"
#include "klringbuffer.h"

#define LOCAL_DEBUG 1

struct ltntstools_segmentwriter_s
{
	time_t lastOpen;
	pthread_t threadId;

	pthread_mutex_t mutex;
	KLRingBuffer *rb;

	/* 0 = Write a single large file, 1 = write 1min segments. */
	int writeMode;

	int threadTerminate, threadTerminated, threadRunning;

	FILE *fh;
	char *filenamePrefix;
	char *filename;
};

static size_t _write(struct ltntstools_segmentwriter_s *s)
{
	time_t now = time(NULL);

	if (s->writeMode == 1 && (s->fh)) {
		if (now >= (s->lastOpen + 60)) {
			fclose(s->fh);
			s->fh = NULL;
		}
	}

	if (s->writeMode == 0) {
		if (s->fh == NULL) {
			if (s->filename)
				free(s->filename);
			s->filename = strdup(s->filenamePrefix);
		}
	} else
	if (s->writeMode == 1) {
		if (s->fh == NULL) {
			char ts[64];
			libltntstools_getTimestamp(&ts[0], sizeof(ts), NULL);
			s->filename = realloc(s->filename, 512);
			sprintf(s->filename, "%s-%s.ts", s->filenamePrefix, ts);
		}
	}

	if (s->fh == NULL) {
#if LOCAL_DEBUG
		printf("%s() Opening %s\n", __func__, s->filename);
#endif
		s->fh = fopen(s->filename, "wb");
		s->lastOpen = time(NULL);
	}

	if (s->fh) {
		pthread_mutex_lock(&s->mutex);
		int len = rb_used(s->rb);
		char *buf = malloc(len);
		rb_read(s->rb, buf, len);
		fwrite(buf, 1, len, s->fh);
		pthread_mutex_unlock(&s->mutex);
	}

	return 0;
}

void *ltntstools_segmentwriter_threadFunc(void *p)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)p;

	s->threadRunning = 1;

	while (!s->threadTerminate) {
		usleep(50 * 1000);
		if (!rb_is_empty(s->rb)) {
			_write(s);
		}
	}

	s->threadRunning = 0;
	s->threadTerminated = 1;
	return NULL;
}

int ltntstools_segmentwriter_alloc(void **hdl, const char *filenamePrefix, int writeMode)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)calloc(1, sizeof(*s));

	pthread_mutex_init(&s->mutex, NULL);
	s->filenamePrefix = strdup(filenamePrefix);
	s->writeMode = writeMode;
	s->rb = rb_new(64 * 1024, 1024 * 1024);

	*hdl = s;

	return pthread_create(&s->threadId, NULL, ltntstools_segmentwriter_threadFunc, s);
}

void ltntstools_segmentwriter_free(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (s->threadRunning) {
		s->threadTerminate = 1;
		while (!s->threadTerminated)
			usleep(5 * 1000);
	}
	free(s->filenamePrefix);
	free(s);
}

ssize_t ltntstools_segmentwriter_write(void *hdl, const uint8_t *buf, size_t length)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;

	pthread_mutex_lock(&s->mutex);
	int didOverflow;
	ssize_t len = rb_write_with_state(s->rb, (const char *)buf, length, &didOverflow);
	pthread_mutex_unlock(&s->mutex);

	return len;
}

