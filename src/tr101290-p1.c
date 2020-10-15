#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "libltntstools/tr101290.h"
#include "libltntstools/time.h"
#include "libltntstools/ts.h"

#include "tr101290-types.h"

#define LOCAL_DEBUG 1

ssize_t p1_write(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	struct timeval now;
	gettimeofday(&now, NULL);

	/* P1.1 is taken care of by the background thread.
	 * It monitors calls to _write, and if they stop, we declare that
	 * TS_SYNC_LOSS. We're generally more flexible in this design because we
	 * aren't dealing with RF< we're dealing with IP networks, and the metric
	 * we truly care about is, the the network stall for more than X ms?
	 */

	/* P1.2 - Sync Byte Error, sync byte != 0x47.
	 * Most TR101290 processors assume this condition rises when P1.1 is bad,
	 * it's not true, especially in a IP network. IN the event of a packet stall,
	 * or jitter, transport is lost for N ms, but resumes perfectly with zero
	 * packet loss, in this case we never want to declare P1.2.
	 */
	for (int i = 0; i < packetCount; i += 188) {
#if ENABLE_TESTING
		FILE *fh = fopen("/tmp/manglesyncbyte", "rb");
		if (fh) {
			unsigned char *p = (unsigned char *)&buf[i];
			*(p + 0) = 0x46;
			fclose(fh);
		}
#endif

		if (ltntstools_sync_present(&buf[i])) {
			s->consecutiveSyncBytes++;
		} else {
			/* Raise */
			s->consecutiveSyncBytes = 0;
			ltntstools_tr101290_alarm_raise(s, E101290_P1_2__SYNC_BYTE_ERROR);
		}
	}

	if (s->consecutiveSyncBytes > 5) {
		/* Clear Alarm */
		struct tr_event_s *ev = &s->event_tbl[E101290_P1_2__SYNC_BYTE_ERROR];

		if (ev->autoClearAlarmAfterReport) {
			struct timeval interval = { ev->autoClearAlarmAfterReport, 0 };
			struct timeval final;
			timeradd(&ev->lastReported, &interval, &final);
			
			if (timercmp(&now, &final, >= )) {
				ltntstools_tr101290_alarm_clear(s, ev->id);
			}
		}
	}
	if (s->consecutiveSyncBytes >= 50000) {
		/* We never want the int to wrap back to zero during long term test. Once we're a certain size,
		 * our wrap point needs to clear a value of zero.
		 */
		s->consecutiveSyncBytes = 16; /* Stay clear of the window where sync byte is cleared. */
	}
	/* End: P1.2 - Sync Byte Error, sync byte != 0x47. */

	/* P1.3 - PAT_error */
	/* End: P1.3 - PAT_error */

	return packetCount;
}

