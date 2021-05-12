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

#define LOCAL_DEBUG 0

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
	char *filenameSuffix;
	char *filename;

	unsigned char *fileHeader;
	int fileHeaderLength;
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
			s->filename = realloc(s->filename, 512);
			sprintf(s->filename, "%s-%s%s", s->filenamePrefix, ts, s->filenameSuffix);
		}
	}

	if (s->fh == NULL) {
#if LOCAL_DEBUG
		printf("%s() Opening %s\n", __func__, s->filename);
#endif
		s->fh = fopen(s->filename, "wb");
		s->lastOpen = time(NULL);
		if (s->fh) {
			fwrite(s->fileHeader, 1, s->fileHeaderLength, s->fh);
		}
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
	s->rb = rb_new(2 * 1048576, 16 * 1048576);

	*hdl = s;

	return pthread_create(&s->threadId, NULL, ltntstools_segmentwriter_threadFunc, s);
}

void ltntstools_segmentwriter_free(void *hdl)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;
	if (s->threadRunning) {
		s->threadTerminate = 1;
	}
	if (s->fileHeader) {
		free(s->fileHeader);
		s->fileHeader = NULL;
		s->fileHeaderLength = 0;
	}
}

ssize_t ltntstools_segmentwriter_write(void *hdl, const uint8_t *buf, size_t length)
{
	struct ltntstools_segmentwriter_s *s = (struct ltntstools_segmentwriter_s *)hdl;

	pthread_mutex_lock(&s->mutex);
	int didOverflow;
	ssize_t len = rb_write_with_state(s->rb, (const char *)buf, length, &didOverflow);
	pthread_mutex_unlock(&s->mutex);

	if (didOverflow) {
		fprintf(stderr, "%s() didOverflow\n", __func__);
	}

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
