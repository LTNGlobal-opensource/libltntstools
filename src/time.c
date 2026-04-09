/* Copyright LiveTimeNet, Inc. 2020. All Rights Reserved. */

#include <stdio.h>
#include "libltntstools/time.h"

int libltntstools_getTimestamp(char *buf, int buflen, time_t *when)
{
	if (buflen < 16)
		return -1;

	struct tm tm;
	time_t now;

	if (when)
		now = *when;
	else
		time(&now);

	localtime_r(&now, &tm);

	sprintf(buf, "%04d%02d%02d-%02d%02d%02d",
		tm.tm_year + 1900,
		tm.tm_mon  + 1,
		tm.tm_mday,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec);

	return 0;
}

int libltntstools_getTimestamp_seperated(char *buf, int buflen, time_t *when)
{
	if (buflen < 16)
		return -1;

	struct tm tm;
	time_t now;

	if (when)
		now = *when;
	else
		time(&now);

	localtime_r(&now, &tm);

	sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
		tm.tm_year + 1900,
		tm.tm_mon  + 1,
		tm.tm_mday,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec);

	return 0;
}

int libltntstools_timespec_diff_ms(struct timespec next_time, struct timespec last_time)
{
	struct timespec diff;
	diff.tv_sec = next_time.tv_sec - last_time.tv_sec;
	diff.tv_nsec = next_time.tv_nsec - last_time.tv_nsec;
	if (diff.tv_nsec < 0) {
		diff.tv_sec -= 1;
		diff.tv_nsec += 1000000000L;
	}

	int ms = (diff.tv_sec * 1000) + (diff.tv_nsec / 1000000);

	return ms;
}
