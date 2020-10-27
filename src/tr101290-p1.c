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

static ssize_t p1_process_p1_5(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	/* PMT checking */
	int complete = 0;
	ltntstools_streammodel_write(s->smHandle, buf, packetCount, &complete);
	return packetCount;
}

static ssize_t p1_process_p1_4(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	uint64_t count = ltntstools_pid_stats_stream_get_cc_errors(&s->streamStatistics);

#if ENABLE_TESTING
	FILE *fh = fopen("/tmp/mangleccbyte", "rb");
	if (fh) {
		s->CCCounterLastWrite = count - 1;
		fclose(fh);
	}
#endif
	/* P1_4: Incorrect packet order */
	if (s->CCCounterLastWrite != count) {
		ltntstools_tr101290_alarm_raise(s, E101290_P1_4__CONTINUITY_COUNTER_ERROR);
	} else {
		/* If the period of time between the last report and this clear is more than
		 * five seconds, automatically clear the alarm.
		 */
		struct tr_event_s *ev = &s->event_tbl[E101290_P1_4__CONTINUITY_COUNTER_ERROR];

		if (ev->autoClearAlarmAfterReport) {
			struct timeval interval = { ev->autoClearAlarmAfterReport, 0 };
			struct timeval final;
			timeradd(&ev->lastReported, &interval, &final);

			struct timeval now;
			gettimeofday(&now, NULL);
			if (timercmp(&now, &final, >= )) {
				ltntstools_tr101290_alarm_clear(s, ev->id);
			}
		}
	}
	s->CCCounterLastWrite = count;

	return packetCount;
}

static ssize_t p1_process_p1_3(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	/* Look for a PAT */
	for (int i = 0; i < packetCount; i++) {
		uint16_t pid = ltntstools_pid(&buf[i * 188]);
		if (pid == 0) {
			/* Good */

			/* PID 0x0000 does not contain a table_id 0x00 */
			unsigned char tableid = ltntstools_get_section_tableid((unsigned char *)&buf[i * 188]);
			if (tableid != 0) {
				ltntstools_tr101290_alarm_raise(s, E101290_P1_3__PAT_ERROR);
				ltntstools_tr101290_alarm_raise(s, E101290_P1_3a__PAT_ERROR_2);
			}

			/* Scrambling_control_field is not 00 for PID 0x0000 */
			if (ltntstools_transport_scrambling_control(&buf[i * 188]) != 0) {
				ltntstools_tr101290_alarm_raise(s, E101290_P1_3__PAT_ERROR);
				ltntstools_tr101290_alarm_raise(s, E101290_P1_3a__PAT_ERROR_2);
			}
		}
	}

	/* PID 0x0000 does not occur at least every 0,5 s
	 * PAT has a time firing every 100ms, if no packets on pid 0 are found, we raise and error....
	 */
	
	return packetCount;
}

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
	for (int i = 0; i < packetCount; i++) {
#if ENABLE_TESTING
		FILE *fh = fopen("/tmp/manglesyncbyte", "rb");
		if (fh) {
			unsigned char *p = (unsigned char *)&buf[i * 188];
			*(p + 0) = 0x46;
			fclose(fh);
		}
#endif

		if (ltntstools_sync_present(&buf[i * 188])) {
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
	p1_process_p1_3(s, buf, packetCount);
	/* End: P1.3 - PAT_error */

	/* P1.4 */
	p1_process_p1_4(s, buf, packetCount);
	/* End: P1.4 */
	
	/* P1.5 */
	p1_process_p1_5(s, buf, packetCount);
	/* End: P1.5 */
	

	return packetCount;
}

