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

/* Called from the P1 layer when a new streammodel arrives every send or so.
 * take a look at the PCR pids and enable those in the general TS stats model,
 * so PCR timing analysis is auto-enabled.
 * We won't own the pat, don't change or free it.
 */
void p2_process_pat_model(struct ltntstools_tr101290_s *s, struct ltntstools_pat_s *pat)
{
	for (int i = 0; i < pat->program_count; i++) {
		if (pat->programs[i].pmt.PCR_PID) {
			ltntstools_pid_stats_pid_set_contains_pcr(&s->streamStatistics, pat->programs[i].pmt.PCR_PID);
		}
	}
}

static int raise_alarm_p2_table(struct ltntstools_tr101290_s *s, struct timeval *tv)
{
	/* Make sure we got a PAT in the last 500ms */
	struct timeval interval = { 0, 500 * 1000 };
	struct timeval window;
	timeradd(tv, &interval, &window);

	/* Check each of the s->p2 times, if we haven't seen a packet in the last N ms, throw an alert */
	if (timercmp(&s->now, &window, >= )) {
		return 1;
	}

	return 0;
}

/* P2.2 - CRC error occurred in CAT, PAT, PMT, NIT, EIT, BAT, SDT or TOT table */
/* See also: ETSI EN 300 468 V1.11.1 (2010-04) Section 5.1.3 */
void p2_process_p2_2(struct ltntstools_tr101290_s *s)
{
	char msg[64] = { 0 };
	int alarmCount = 0;

	return; /* Not specific check to ensure these are delivered on time in p2.2 */

	if (raise_alarm_p2_table(s, &s->p2.lastPAT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "PAT ");
	}
	if (raise_alarm_p2_table(s, &s->p2.lastPMT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "PMT ");
	}
	if (raise_alarm_p2_table(s, &s->p2.lastCAT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "CAT ");
	}
	if (raise_alarm_p2_table(s, &s->p2.lastSDT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "SDT ");
	}
	if (raise_alarm_p2_table(s, &s->p2.lastBAT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "BAT ");
	}
	if (raise_alarm_p2_table(s, &s->p2.lastNIT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "NIT ");
	}
	if (raise_alarm_p2_table(s, &s->p2.lastTOT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "TOT ");
	}
	if (raise_alarm_p2_table(s, &s->p2.lastEIT)) {
		alarmCount++;
		sprintf(msg + strlen(msg), "EIT ");
	}

	if (alarmCount) {
		ltntstools_tr101290_alarm_raise_with_arg(s, E101290_P2_2__CRC_ERROR, msg);
	}

}

/* P2.3 -
 * PCR discontinuity of more than 100 ms occurring without specific indication.
 * Time interval between two consecutive PCR values more than 40 ms.
 */
static void p2_process_p2_3(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	char msg[128];
	msg[0] = 0;
	int raiseIssue = 0;

	int arrCount = 0;
	uint16_t arr[32];

	if (packetCount >= 31) {
		printf("%s() tr101290 framework - avoiding overrun, fix me\n", __func__);
		return;
	}

	for (int i = 0; i < packetCount; i++) {
		const uint8_t *pkt = &buf[i * 188];

		uint16_t pid = ltntstools_pid(pkt);
		if (!ltntstools_pid_stats_pid_get_contains_pcr(&s->streamStatistics, pid))
			continue;

		/* Check the stats */
		if (ltntstools_pid_stats_pid_did_violate_pcr_timing(&s->streamStatistics, pid)) {

			/* Pretty common to have 7 video packets in a single buffer, avoid raising the
			 * alarm multiple times for the same pid.
			 */
			int found = 0;
			for (int j = 0; j < arrCount; j++) {
				if (pid == arr[j]) {
					found++;
					break;
				}
			}

			if (!found) {
				sprintf(msg + strlen(msg), "0x%04x ", pid);
				raiseIssue++;
				arr[arrCount++] = pid;
			}
		}
	}

	if (raiseIssue) {
		ltntstools_tr101290_alarm_raise_with_arg(s, E101290_P2_3__PCR_ERROR, msg);
		ltntstools_tr101290_alarm_raise_with_arg(s, E101290_P2_3a__PCR_REPETITION_ERROR, msg);
	} else {
		ltntstools_tr101290_alarm_clear(s, E101290_P2_3__PCR_ERROR);
		ltntstools_tr101290_alarm_clear(s, E101290_P2_3a__PCR_REPETITION_ERROR);
	}
}

/* P2.2 - CRC error occurred in CAT, PAT, PMT, NIT, EIT, BAT, SDT or TOT table */
/* See also: ETSI EN 300 468 V1.11.1 (2010-04) Section 5.1.3 */
void *p2_streammodel_callback(void *userContext, struct streammodel_callback_args_s *args)
{
	struct ltntstools_tr101290_s *s = (struct ltntstools_tr101290_s *)userContext;
#if 0
printf("status  %d  ", args->status);
printf("context %d  ", args->context);
printf("arg %d\n", args->arg);
#endif
	if (args->status != STREAMMODEL_CB_CRC_STATUS) {
		return NULL;
	}

	uint32_t id = E101290_P2_2__CRC_ERROR;

	switch (args->context) {
	case STREAMMODEL_CB_CONTEXT_PAT:
		s->p2.lastPAT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "PAT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	case STREAMMODEL_CB_CONTEXT_PMT:
		s->p2.lastPMT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "PMT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	case STREAMMODEL_CB_CONTEXT_CAT:
		s->p2.lastCAT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "CAT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	case STREAMMODEL_CB_CONTEXT_SDT:
		s->p2.lastSDT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "SDT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	case STREAMMODEL_CB_CONTEXT_BAT:
		s->p2.lastBAT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "BAT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	case STREAMMODEL_CB_CONTEXT_NIT:
		s->p2.lastNIT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "NIT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	case STREAMMODEL_CB_CONTEXT_TOT:
		s->p2.lastTOT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "TOT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	case STREAMMODEL_CB_CONTEXT_EIT:
		s->p2.lastEIT = s->now;
		if (args->arg == CRC_ARG_INVALID)
			ltntstools_tr101290_alarm_raise_with_arg(s, id, "EIT");
		else
			ltntstools_tr101290_alarm_clear(s, id);
		break;
	}

	return NULL; /* Success */
}

ssize_t p2_write(struct ltntstools_tr101290_s *s, const uint8_t *buf, size_t packetCount)
{
	struct timespec now_ts;
	clock_gettime(CLOCK_REALTIME, &now_ts);
	struct timeval now = timespec_to_timeval(&now_ts);

	/* P2.1 - Transport_Error TEI bit set. */
	if (s->preTEIErrors != ltntstools_pid_stats_stream_get_tei_errors(&s->streamStatistics)) {
		ltntstools_tr101290_alarm_raise(s, E101290_P2_1__TRANSPORT_ERROR);
	} else {
		ltntstools_tr101290_alarm_clear(s, E101290_P2_1__TRANSPORT_ERROR);
	}

	/* P2.6 - Packets with transport_scrambling_control not 00 present, but no section with table_id = 0x01 (i.e. a CAT) present. */
	/* TODO: Read the clause. This clears the alrt if ANY CAT table arrives. In an MPTS configuration this might not be
	 * tight enough, we need to check ALL CAT tables.
	 */
	if (raise_alarm_p2_table(s, &s->p2.lastCAT)) {
		/* We haven't seen a CAT table in a while. */
		if (s->preScrambledCount != ltntstools_pid_stats_stream_get_scrambled_count(&s->streamStatistics)) {
			/* New scrambled packets have arrived */
			ltntstools_tr101290_alarm_raise(s, E101290_P2_6__CAT_ERROR);
		} else {
			ltntstools_tr101290_alarm_clear(s, E101290_P2_6__CAT_ERROR);
		}
	} else {
		ltntstools_tr101290_alarm_clear(s, E101290_P2_6__CAT_ERROR);
	}

	p2_process_p2_3(s, buf, packetCount);

	return packetCount;
}

