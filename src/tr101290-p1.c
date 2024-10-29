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

/* Has a given pid been updated in the last 2 second?
 * 1 on success, else not.
 */
static int isPIDActive(struct ltntstools_tr101290_s *s, uint16_t pidNr, time_t now)
{
	time_t ptv = ltntstools_pid_stats_pid_get_last_update(&s->streamStatistics, pidNr);
	if (ptv <= (now - 5)) {
		//printf("0x%04x ptv %d\n", pidNr, ptv);
		return 0;
	}

	return 1; /* Pid is Active */
}

/* P1.6 - Referred PID does not occur for a user specified period. */
/* We're called with a valid stream model. */
static void p1_process_p1_6(struct ltntstools_tr101290_s *s, struct ltntstools_pat_s *pat, time_t now)
{
	/* Walk each pid in the model, make sure we've received a packet in the last 1000ms
	 * or less, otherwise raise an error.
	 */

	int raiseIssue = 0;
	char msg[128];
	msg[0] = 0;

	for (int i = 0; i < pat->program_count; i++) {
		struct ltntstools_pat_program_s *prg = &pat->programs[i];

		/* Specifically, don't skip the network pid program_number 0 */

		/* Check the PMT */
		if (isPIDActive(s, prg->program_map_PID, now) == 0) {
			sprintf(msg + strlen(msg), "0x%04x ", prg->program_map_PID);
			raiseIssue++;
		}

		/* Check every ES in the PMT. */
		for (int j = 0; j < prg->pmt.stream_count; j++) {
			struct ltntstools_pmt_entry_s *strm = &prg->pmt.streams[j];

			/* Check each ES */
			if (isPIDActive(s, strm->elementary_PID, now) == 0) {
				sprintf(msg + strlen(msg), "0x%04x ", strm->elementary_PID);
				raiseIssue++;
			}

		}
	}

	if (!raiseIssue) {
		ltntstools_tr101290_alarm_clear(s, E101290_P1_6__PID_ERROR);
	} else {
		ltntstools_tr101290_alarm_raise_with_arg(s, E101290_P1_6__PID_ERROR, msg);
	}

}

static ssize_t p1_process_p1_56(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	/* Sections with table_id 0x02, (i.e. a PMT), do not occur at least every 0,5 s on the PID which is
	 * referred to in the PAT.
	 * Scrambling_control_field is not 00 for all PIDs containing sections with table_id 0x02 (i.e. a PMT)
	*/

	/* PMT checking */
	int complete = 0;
	ltntstools_streammodel_write(s->smHandle, buf, packetCount, &complete);

	/* If the stream model is completing, then the PMT's must be ok.
	 * DVBPSI enforces the scrambling control check.
	 */
	//printf("complete %d\n", complete);
	if (complete) {
		time_t now = time(NULL);

		if (s->lastCompleteTime < now) {
			s->lastCompleteTime = now;

			ltntstools_tr101290_alarm_clear(s, E101290_P1_5__PMT_ERROR);
			ltntstools_tr101290_alarm_clear(s, E101290_P1_5a__PMT_ERROR_2);

			/* Performance issue. Calling query_model is too expensive to happen on
			 * every transport packet write. Make sure we cache the results then age them out
			 * over time. It's fine to do this once per second.
			 */

			struct ltntstools_pat_s *pat;
			if (ltntstools_streammodel_query_model(s->smHandle, &pat) == 0) {

				/* Some of the P2 PCR checks need to know which pics have PCRs.
				 * Give the P2 processor a sneak peak at the model.
				 */
				p2_process_pat_model(s, pat);

				/* Check 1.6 Now that we have a stream model. */
				p1_process_p1_6(s, pat, now);
				free(pat);
			}
		}

	}

	/* TODO: What should we be doing here? */

	return packetCount;
}

/* P1_4: Incorrect packet order */
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
		ltntstools_tr101290_alarm_clear(s, E101290_P1_4__CONTINUITY_COUNTER_ERROR);
	}
	s->CCCounterLastWrite = count;

	return packetCount;
}

/* P1.3 - PAT_error */
static ssize_t p1_process_p1_3(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	/* Look for a PAT */
	int patIssue = 0;
	int gotPAT = 0;
	for (int i = 0; i < packetCount; i++) {
		uint16_t pid = ltntstools_pid(&buf[i * 188]);
		if (pid == 0) {
			/* Good */
			gotPAT = 1;
			
			/* PID 0x0000 does not contain a table_id 0x00 */
			unsigned char tableid = ltntstools_get_section_tableid((unsigned char *)&buf[i * 188]);
			if (tableid != 0) {
				patIssue++;
			}

			/* Scrambling_control_field is not 00 for PID 0x0000 */
			if (ltntstools_transport_scrambling_control(&buf[i * 188]) != 0) {
				patIssue++;
			}
		}
	}

	if (!gotPAT) {
		/* Make sure we got a PAT in the last 500ms */
		struct timeval interval = { 0, 500 * 1000 };
		struct timeval window;
		timeradd(&s->lastPAT, &interval, &window);
		if (timercmp(&s->now, &window, >= )) {
			patIssue++;
		}
	} else {
		s->lastPAT = s->now;
	}

	if (patIssue) {
		ltntstools_tr101290_alarm_raise(s, E101290_P1_3__PAT_ERROR);
		ltntstools_tr101290_alarm_raise(s, E101290_P1_3a__PAT_ERROR_2);
	} else {
		ltntstools_tr101290_alarm_clear(s, E101290_P1_3__PAT_ERROR);
		ltntstools_tr101290_alarm_clear(s, E101290_P1_3a__PAT_ERROR_2);
	}

	/* PID 0x0000 does not occur at least every 0,5 s
	 * PAT has a time firing every 100ms, if no packets on pid 0 are found, we raise and error....
	 */
	
	return packetCount;
}

ssize_t p1_write(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	struct timespec now_ts;
	clock_gettime(CLOCK_REALTIME, &now_ts);
	struct timeval now = timespec_to_timeval(&now_ts);

	/* P1.1 is taken care of by the background thread.
	 * It monitors calls to _write, and if they stop, we declare that
	 * TS_SYNC_LOSS. We're generally more flexible in this design because we
	 * aren't dealing with RF, we're dealing with IP networks, and the metric
	 * we truly care about is, the the network stall for more than X ms?
	 */

	/* P1.2 - Sync Byte Error, sync byte != 0x47.
	 * Most TR101290 processors assume this condition rises when P1.1 is bad,
	 * it's not true, especially in a IP network. In the event of a packet stall,
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
		ltntstools_tr101290_alarm_clear(s, E101290_P1_2__SYNC_BYTE_ERROR);
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
	
	/* P1.5/6 */
	p1_process_p1_56(s, buf, packetCount);
	/* End: P1.5/6 */
	
	return packetCount;
}

