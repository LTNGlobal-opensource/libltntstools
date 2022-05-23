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

	sprintf(buf, "%04d/%02d/%02d-%02d:%02d:%02d",
		tm.tm_year + 1900,
		tm.tm_mon  + 1,
		tm.tm_mday,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec);

	return 0;
}
