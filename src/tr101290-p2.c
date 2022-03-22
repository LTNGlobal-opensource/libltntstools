#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "libltntstools/ltntstools.h"

#include "tr101290-types.h"

#define LOCAL_DEBUG 1

ssize_t p2_write(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	struct timeval now;
	gettimeofday(&now, NULL);

	/* P2.1 - Transport_Error TEI bit set. */
	if (s->preTEIErrors != ltntstools_pid_stats_stream_get_tei_errors(&s->streamStatistics)) {
		ltntstools_tr101290_alarm_raise(s, E101290_P2_1__TRANSPORT_ERROR);
	} else {
		ltntstools_tr101290_alarm_clear(s, E101290_P2_1__TRANSPORT_ERROR);
	}

	for (int i = 0; i < packetCount; i++) {
#if ENABLE_TESTING
		FILE *fh = fopen("/tmp/mangleteibyte", "rb");
		if (fh) {
			unsigned char *p = (unsigned char *)&buf[i * 188];
			*(p + 0) = 0x46;
			fclose(fh);
		}
#endif

	} /* For */

	return packetCount;
}

