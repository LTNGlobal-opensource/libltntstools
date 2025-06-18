/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include "libltntstools/ltntstools.h"

/* Forward defines */
uint32_t ltntstools_cc_reorder_table_readpos(struct ltntstools_cc_reorder_table_s *t, int offset);
void     ltntstools_cc_reorder_table_add(struct ltntstools_cc_reorder_table_s *t, uint16_t pid, uint8_t cc, int isCCError);
void     ltntstools_cc_reorder_table_sum(struct ltntstools_cc_reorder_table_s *t, uint32_t *value, uint32_t *ccerrors);
void     ltntstools_cc_reorder_table_print(struct ltntstools_cc_reorder_table_s *t);
int      ltntstools_cc_reorder_table_corelate(struct ltntstools_cc_reorder_table_s *t);
void     ltntstools_cc_reorder_table_reset(struct ltntstools_cc_reorder_table_s *t);
static void _stream_increment_cc_errors(struct ltntstools_stream_statistics_s *stream, struct timeval *ts);

const char *ltntstools_notification_event_name(enum ltntstools_notification_event_e e)
{
	switch(e) {
	case EVENT_UNDEFINED:                     return "EVENT_UNDEFINED";
	case EVENT_UPDATE_PID_PUSI_DELIVERY_TIME: return "EVENT_UPDATE_PID_PUSI_DELIVERY_TIME";
	case EVENT_UPDATE_PID_PCR_EXCEEDS_40MS:   return "EVENT_UPDATE_PID_PCR_EXCEEDS_40MS";
	case EVENT_UPDATE_PID_PCR_WALLTIME:       return "EVENT_UPDATE_PID_PCR_WALLTIME";
	case EVENT_UPDATE_STREAM_CC_COUNT:        return "EVENT_UPDATE_STREAM_CC_COUNT";
	case EVENT_UPDATE_STREAM_TEI_COUNT:       return "EVENT_UPDATE_STREAM_TEI_COUNT";
	case EVENT_UPDATE_STREAM_SCRAMBLED_COUNT: return "EVENT_UPDATE_STREAM_SCRAMBLED_COUNT";
	case EVENT_UPDATE_STREAM_MBPS:            return "EVENT_UPDATE_STREAM_MBPS";
	case EVENT_UPDATE_STREAM_IAT_HWM:         return "EVENT_UPDATE_STREAM_IAT_HWM";
	default:                                  return "EVENT_UNKNOWN";
	}
}

int ltntstools_notification_register_callback(struct ltntstools_stream_statistics_s *stream, enum ltntstools_notification_event_e e,
	void *userContext, ltntstools_notification_callback cb)
{
	if (cb == NULL) {
		return -1;
	}

	/* Null user contexts are allowable */

	stream->notifications[e].cb = cb;
	stream->notifications[e].userContext = userContext;
	return 0; /* Success */
}

void ltntstools_notification_unregister_callback(struct ltntstools_stream_statistics_s *stream, enum ltntstools_notification_event_e e)
{
	stream->notifications[e].cb = NULL;
	stream->notifications[e].userContext = NULL;
}

int ltntstools_isCCInError(const uint8_t *pkt, uint8_t oldCC)
{
	unsigned int adap = ltntstools_adaption_field_control(pkt);
	unsigned int cc = ltntstools_continuity_counter(pkt);

	if (((adap == 0) || (adap == 2)) && (oldCC == cc))
		return 0;

	if (((adap == 1) || (adap == 3)) && (oldCC == cc))
		return 1;

	if (((oldCC + 1) & 0x0f) == cc)
		return 0;

	return 1;
}

void ltntstools_bytestream_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *buf, uint32_t lengthBytes)
{
	time_t now;
	time(&now);

	stream->packetCount++;
	if (lengthBytes != (7 * 188)) {
		stream->notMultipleOfSevenError++;
		stream->last_notMultipleOfSeven_error = now;
	}

	/* Update / maintain bitrate */
	if (now != stream->Bps_last_update) {
		stream->Bps = stream->Bps_window;
		stream->Bps_window = 0;
		stream->a324_bps = stream->Bps * 8;
		stream->a324_mbps = stream->Bps * 8;
		stream->a324_mbps /= 1e6;
		stream->Bps_last_update = now;
	}
	stream->Bps_window += lengthBytes;
}

void ltntstools_ctp_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *buf, uint32_t lengthBytes)
{
	time_t now;
	time(&now);

	if (lengthBytes != (7 * 188)) {
		stream->notMultipleOfSevenError++;
		stream->last_notMultipleOfSeven_error = now;
	}

	/* Pull the CC out of the frame and check for CC loss. */
	uint16_t sequence_number = *(buf + 2) << 8 | *(buf + 3);
	if (((stream->a324_sequence_number + 1) & 0xffff) != sequence_number) {
		/* No CC error for the first packet. */
		if (stream->packetCount) {
			_stream_increment_cc_errors(stream, NULL);
		}
	}
	stream->a324_sequence_number = sequence_number;

	/* Roughly convert the CTL into number of packets so we can count them, approximately correct.
	 * But some rounding down will occur.
	 * TODO: Add a CTP correct mechanism.
	 */
	stream->packetCount++;

	/* Update / maintain bitrate */
	if (now != stream->Bps_last_update) {
		stream->Bps = stream->Bps_window;
		stream->Bps_window = 0;
		stream->a324_bps = stream->Bps * 8;
		stream->a324_mbps = stream->Bps * 8;
		stream->a324_mbps /= 1e6;
		stream->Bps_last_update = now;
	}
	stream->Bps_window += lengthBytes;
}

static void _stream_increment_cc_errors(struct ltntstools_stream_statistics_s *stream, struct timeval *ts)
{
	struct timeval now;
	if (ts) {
		now = *ts;
	} else {
		gettimeofday(&now, NULL);
	}

	stream->ccErrors++;
	stream->last_cc_error = now.tv_sec;

	if (stream->notifications[EVENT_UPDATE_STREAM_CC_COUNT].cb) {
		stream->notifications[EVENT_UPDATE_STREAM_CC_COUNT].cb(stream->notifications[EVENT_UPDATE_STREAM_CC_COUNT].userContext, 
			EVENT_UPDATE_STREAM_CC_COUNT, stream, NULL);
	}
}

void ltntstools_pid_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *pkts, uint32_t packetCount)
{
	struct timeval ts;
	gettimeofday(&ts, NULL);

	time_t now;
	time(&now);

	if (packetCount != 7) {
		stream->notMultipleOfSevenError++;
		stream->last_notMultipleOfSeven_error = now;
	}

	for (int i = 0; i < packetCount; i++) {
		int offset = i * 188;
		if (*(pkts + offset) == 0x47)
			stream->packetCount++;
		else {
			_stream_increment_cc_errors(stream, &ts);
		}
	}

	if (now != stream->pps_last_update) {
		stream->pps = stream->pps_window;
		stream->pps_window = 0;
		stream->mbps = stream->pps;
		stream->mbps *= (188 * 8);
		stream->mbps /= 1e6;
		stream->pps_last_update = now;

		if (stream->notifications[EVENT_UPDATE_STREAM_MBPS].cb) {
			stream->notifications[EVENT_UPDATE_STREAM_MBPS].cb(stream->notifications[EVENT_UPDATE_STREAM_MBPS].userContext, 
				EVENT_UPDATE_STREAM_MBPS, stream, NULL);
		}
	}
	stream->pps_window += packetCount;

	if (stream->iat_last_frame.tv_sec) {
		stream->iat_cur_us = ltn_timeval_subtract_us(&ts, &stream->iat_last_frame);
		if (stream->iat_cur_us <= stream->iat_lwm_us)
			stream->iat_lwm_us = stream->iat_cur_us;
		if (stream->iat_cur_us >= stream->iat_hwm_us) {
			stream->iat_hwm_us = stream->iat_cur_us;
			if (stream->notifications[EVENT_UPDATE_STREAM_IAT_HWM].cb) {
				stream->notifications[EVENT_UPDATE_STREAM_IAT_HWM].cb(stream->notifications[EVENT_UPDATE_STREAM_IAT_HWM].userContext, 
					EVENT_UPDATE_STREAM_IAT_HWM, stream, NULL);
			}
		}

		/* Track max IAT for the last N seconds, it's reported in the summary/detailed logs. */
		if (stream->iat_cur_us > stream->iat_hwm_us_last_nsecond_accumulator) {
			stream->iat_hwm_us_last_nsecond_accumulator = stream->iat_cur_us;
		}
		if ((stream->iat_hwm_us_last_nsecond_time + 5 /* seconds */) <= now) {
			stream->iat_hwm_us_last_nsecond_time = now;
			stream->iat_hwm_us_last_nsecond = stream->iat_hwm_us_last_nsecond_accumulator;
			stream->iat_hwm_us_last_nsecond_accumulator = 0;
		}

		ltn_histogram_interval_update_with_value(stream->packetIntervals, stream->iat_cur_us / 1000);
	}

	for (int i = 0; i < packetCount; i++) {
		int offset = i * 188;

		uint16_t pidnr = ltntstools_pid(pkts + offset);
		struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr];

		pid->enabled = 1;
		pid->packetCount++;

		/* Per pid, of PUSI is being used, track the time we see the header
		 * and the time we see the last write on this pid (pusi_time_current).
		 * When a PUSI arrives, calculate the difference and make it available.
		 */
		if (ltntstools_payload_unit_start_indicator(pkts + offset)) {

			if (pid->pusi_time_first.tv_sec && pid->pusi_time_current.tv_sec) {
				/* don't push a stat to the suer that's only partial incorrect because time isn't fully esatblished.
				 * They'll end up with a massive irrelvant ms delivery time.
				 */
				pid->pusi_time_ms = ltn_timeval_subtract_ms(&pid->pusi_time_current, &pid->pusi_time_first);

				if (stream->notifications[EVENT_UPDATE_PID_PUSI_DELIVERY_TIME].cb) {
					stream->notifications[EVENT_UPDATE_PID_PUSI_DELIVERY_TIME].cb(stream->notifications[EVENT_UPDATE_PID_PUSI_DELIVERY_TIME].userContext, 
						EVENT_UPDATE_PID_PUSI_DELIVERY_TIME, stream, pid);
				}

			}

			pid->pusi_time_first = ts; /* And the process resets collection again */
		}
		pid->pusi_time_current = ts;

		if (0 && pid->packetCount == 1) { /* DISABLED */
			/* Initialize the packet re-order table for the discovered pid */
			pid->reorderTable = calloc(1, sizeof(struct ltntstools_cc_reorder_table_s));			
		}

		if (now != pid->pps_last_update) {
			pid->pps = pid->pps_window;
			pid->pps_window = 0;
			pid->mbps = pid->pps;
			pid->mbps *= (188 * 8);
			pid->mbps /= 1e6;
			pid->pps_last_update = now;
		}
		pid->pps_window++;

		uint8_t cc = ltntstools_continuity_counter(pkts + offset);
		int isCCError = ltntstools_isCCInError(pkts + offset, pid->lastCC);
		if (isCCError) {
			if (pid->packetCount > 1 && pidnr != 0x1fff) {
				pid->ccErrors++;
				_stream_increment_cc_errors(stream, &ts);
			}
		}

		if (pid->reorderTable) {
			ltntstools_cc_reorder_table_add(pid->reorderTable, pidnr, cc, isCCError);
			stream->reorderErrors += ltntstools_cc_reorder_table_corelate(pid->reorderTable);
		}

		uint8_t sc = ltntstools_transport_scrambling_control(pkts + offset);
		if (sc != 0) {
			pid->scrambledCount++;
			stream->scrambledCount++;

			if (stream->notifications[EVENT_UPDATE_STREAM_SCRAMBLED_COUNT].cb) {
				stream->notifications[EVENT_UPDATE_STREAM_SCRAMBLED_COUNT].cb(stream->notifications[EVENT_UPDATE_STREAM_SCRAMBLED_COUNT].userContext, 
					EVENT_UPDATE_STREAM_SCRAMBLED_COUNT, stream, pid);
			}

		}

		pid->lastCC = cc;

		if (ltntstools_tei_set(pkts + offset)) {
			pid->teiErrors++;
			stream->teiErrors++;
			if (stream->notifications[EVENT_UPDATE_STREAM_TEI_COUNT].cb) {
				stream->notifications[EVENT_UPDATE_STREAM_TEI_COUNT].cb(stream->notifications[EVENT_UPDATE_STREAM_TEI_COUNT].userContext, 
					EVENT_UPDATE_STREAM_TEI_COUNT, stream, pid);
			}
		}

		/* If the buffer contains a packet with a potential PCR, and the user has asked
		 * then process the PCR timing.
		 */
		if (pid->hasPCR) {
			/* If the clock is not yet established. */
			struct ltntstools_clock_s *pcrclk = &pid->clocks[ltntstools_CLOCK_PCR];
			/* Attempt to extract a PCR from this packet. */
			uint64_t pcr;
			if (ltntstools_scr((uint8_t *)(pkts + offset), &pcr) == 0) {
				if (pid->seenPCR++ < 100)
					continue;

				if (ltntstools_clock_is_established_timebase(pcrclk) == 0) {
					ltntstools_clock_initialize(pcrclk);
					ltntstools_clock_establish_timebase(pcrclk, 27 * 1e6);

					/* One time initialzation of our histograms. */
					char title[64];
					sprintf(title, "PCR Tick Intervals PID 0x%04x", pidnr);
					ltn_histogram_alloc_video_defaults(&pid->pcrTickIntervals, title);

					sprintf(title, "PCR Jitter PID 0x%04x (abs value)", pidnr);
					ltn_histogram_alloc_video_defaults(&pid->pcrWallDrift, title);
				}

				if (ltntstools_clock_is_established_wallclock(pcrclk) == 0) {
					ltntstools_clock_establish_wallclock(pcrclk, pcr);
				}

				/* Compute the interval in ticks, raise stats errors if they exceeed.
				 * a) 100ms without a stated discontinuity or
				 * b) 40ms.
				 */
				int64_t delta = ltntstools_clock_compute_delta(pcrclk, pcr, ltntstools_clock_get_ticks(pcrclk));
				pid->prev_pcrExceeds40ms = pid->pcrExceeds40ms;
				stream->prev_pcrExceeds40ms = stream->pcrExceeds40ms;
				if (delta > (27000 * 40)) {
					pid->pcrExceeds40ms++;
					stream->pcrExceeds40ms++;
				}

				ltn_histogram_interval_update_with_value(pid->pcrTickIntervals, delta / 27000);

				/* Update current value and re-compute drifts. */
				ltntstools_clock_set_ticks(pcrclk, pcr);
				ltntstools_clock_get_drift_us(pcrclk);

				int64_t v = ltntstools_clock_get_drift_us(pcrclk) / 1000; /* In ms */
				pid->lastPCRWalltimeDriftMs = v;

				/* Normalize to remove drift direction - needed for histogram */
				v = abs(v);
				//printf("us %" PRIi64 "\n", v);
				ltn_histogram_interval_update_with_value(pid->pcrWallDrift, v);

				if (stream->notifications[EVENT_UPDATE_PID_PCR_WALLTIME].cb) {
					stream->notifications[EVENT_UPDATE_PID_PCR_WALLTIME].cb(stream->notifications[EVENT_UPDATE_PID_PCR_WALLTIME].userContext, 
						EVENT_UPDATE_PID_PCR_WALLTIME, stream, pid);
				}
				if (stream->notifications[EVENT_UPDATE_PID_PCR_EXCEEDS_40MS].cb && delta > (27000 * 40)) {
					stream->notifications[EVENT_UPDATE_PID_PCR_EXCEEDS_40MS].cb(stream->notifications[EVENT_UPDATE_PID_PCR_EXCEEDS_40MS].userContext, 
						EVENT_UPDATE_PID_PCR_EXCEEDS_40MS, stream, pid);
				}

			}
		}

	} /* for each ts packet */
	stream->iat_last_frame = ts;
}

void ltntstools_pid_stats_reset(struct ltntstools_stream_statistics_s *stream)
{
	stream->packetCount = 0;
	stream->teiErrors = 0;
	stream->ccErrors = 0;
	stream->last_cc_error = 0;
	stream->mbps = 0;
	stream->reorderErrors = 0;
	stream->notMultipleOfSevenError = 0;
	stream->last_notMultipleOfSeven_error = 0;
	stream->iat_lwm_us = 50000000;
	stream->iat_hwm_us = -1;
	stream->iat_cur_us = 0;

	ltn_histogram_reset(stream->packetIntervals);

	for (int i = 0; i < MAX_PID; i++) {
		stream->pids[i].pidNr = i;
		if (!stream->pids[i].enabled)
			continue;
		stream->pids[i].packetCount = 0;
		stream->pids[i].ccErrors = 0;
		stream->pids[i].teiErrors = 0;
		stream->pids[i].mbps = 0;

		stream->pids[i].clocks[ltntstools_CLOCK_PCR].drift_us_hwm = 0;
		stream->pids[i].clocks[ltntstools_CLOCK_PCR].drift_us_lwm = 0;
		stream->pids[i].clocks[ltntstools_CLOCK_PCR].establishedWT = 0;
		stream->pids[i].seenPCR = 0;

		if (stream->pids[i].pcrTickIntervals) {
			ltn_histogram_reset(stream->pids[i].pcrTickIntervals);
		}
		if (stream->pids[i].pcrWallDrift) {
			ltn_histogram_reset(stream->pids[i].pcrWallDrift);
		}
		if (stream->pids[i].reorderTable) {
			ltntstools_cc_reorder_table_reset(stream->pids[i].reorderTable);
		}
	}
}

int ltntstools_pid_stats_alloc(struct ltntstools_stream_statistics_s **ctx)
{
	*ctx = NULL;

	struct ltntstools_stream_statistics_s *stream = calloc(1, sizeof(*stream));
	if (!stream)
		return -1;

	ltn_histogram_alloc_video_defaults(&stream->packetIntervals, "IAT Intervals");
	ltntstools_pid_stats_reset(stream);

	*ctx = stream;
	return 0;
}

void ltntstools_pid_stats_free(struct ltntstools_stream_statistics_s *stream)
{
	if (!stream || !stream->pids)
		return;

	if (stream->packetIntervals) {
		ltn_histogram_free(stream->packetIntervals);
		stream->packetIntervals = NULL;
	}

	for (int i = 0; i < MAX_PID; i++) {
		if (!stream->pids[i].enabled)
			continue;
		if (stream->pids[i].pcrTickIntervals) {
			ltn_histogram_free(stream->pids[i].pcrTickIntervals);
			stream->pids[i].pcrTickIntervals = NULL;
		}
		if (stream->pids[i].pcrWallDrift) {
			ltn_histogram_free(stream->pids[i].pcrWallDrift);
			stream->pids[i].pcrWallDrift = NULL;
		}
		if (stream->pids[i].reorderTable) {
			free(stream->pids[i].reorderTable);
			stream->pids[i].reorderTable = NULL;
		}
	}

	free(stream);
}

struct ltntstools_stream_statistics_s * ltntstools_pid_stats_clone(struct ltntstools_stream_statistics_s *src)
{
	struct ltntstools_stream_statistics_s *dst = malloc(sizeof(*src));
	if (!dst)
		return NULL;

	memcpy(dst, src, sizeof(*dst));

	return dst;
}

static void _expire_per_second_stream_stats(struct ltntstools_stream_statistics_s *stream)
{
	time_t now;
	time(&now);

	if (stream->Bps_window && now > stream->Bps_last_update + 2) {
		stream->a324_mbps = 0;
		stream->Bps = 0;
		stream->Bps_window = 0;
	} else
	if (now > stream->pps_last_update + 2) {
		stream->mbps = 0;
		stream->pps = 0;
		stream->pps_window = 0;
	}
}

double ltntstools_bytestream_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream)
{
	_expire_per_second_stream_stats(stream);
	return stream->a324_mbps;
}

double ltntstools_ctp_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream)
{
	_expire_per_second_stream_stats(stream);
	return stream->a324_mbps;
}

double ltntstools_pid_stats_stream_get_mbps(struct ltntstools_stream_statistics_s *stream)
{
	_expire_per_second_stream_stats(stream);
	return stream->mbps;
}

uint64_t ltntstools_pid_stats_stream_get_reorder_errors(struct ltntstools_stream_statistics_s *stream)
{
	return stream->reorderErrors;
}

uint32_t ltntstools_pid_stats_stream_get_pps(struct ltntstools_stream_statistics_s *stream)
{
	_expire_per_second_stream_stats(stream);
	return stream->pps;
}

uint32_t ltntstools_ctp_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream)
{
	_expire_per_second_stream_stats(stream);
	return stream->a324_bps;
}

uint32_t ltntstools_bytestream_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream)
{
	_expire_per_second_stream_stats(stream);
	return stream->a324_bps;
}

uint32_t ltntstools_pid_stats_stream_get_bps(struct ltntstools_stream_statistics_s *stream)
{
	_expire_per_second_stream_stats(stream);
	return stream->pps * 188 * 8;
}

static void _expire_per_second_pid_stats(struct ltntstools_pid_statistics_s *pid)
{
	time_t now;
	time(&now);

	if (now > pid->pps_last_update + 2) {
		pid->mbps = 0;
		pid->pps = 0;
		pid->pps_window = 0;
	}
}

double ltntstools_pid_stats_pid_get_mbps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	_expire_per_second_pid_stats(pid);
	return pid->mbps;
}

uint32_t ltntstools_pid_stats_pid_get_pps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	_expire_per_second_pid_stats(pid);
	return pid->pps;
}

uint32_t ltntstools_pid_stats_pid_get_bps(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	_expire_per_second_pid_stats(pid);
	return pid->pps * 188 * 8;
}

uint32_t ltntstools_pid_stats_stream_padding_pct(struct ltntstools_stream_statistics_s *stream)
{
	uint32_t null_bps = ltntstools_pid_stats_pid_get_bps(stream, 0x1fff);
	uint32_t stream_bps = ltntstools_pid_stats_stream_get_bps(stream);

	if (stream_bps == 0)
		return 0;

	return (null_bps * 100) / stream_bps;
}

uint64_t ltntstools_pid_stats_pid_get_packet_count(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	return pid->packetCount;
}

void ltntstools_pid_stats_pid_set_contains_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	pid->hasPCR = 1;
}

int ltntstools_pid_stats_pid_get_contains_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	return pid->hasPCR;
}

int64_t ltntstools_pid_stats_pid_get_pcr(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];

	int64_t pcr = 0;
	if (pid->hasPCR) {
		struct ltntstools_clock_s *pcrclk = &pid->clocks[ltntstools_CLOCK_PCR];
		pcr = ltntstools_clock_get_ticks(pcrclk);
	}
	return pcr;
}

uint64_t ltntstools_pid_stats_stream_get_cc_errors(struct ltntstools_stream_statistics_s *stream)
{
	return stream->ccErrors;
}

time_t ltntstools_pid_stats_stream_get_cc_error_time(struct ltntstools_stream_statistics_s *stream)
{
	return stream->last_cc_error;
}

uint64_t ltntstools_pid_stats_stream_get_tei_errors(struct ltntstools_stream_statistics_s *stream)
{
	return stream->teiErrors;
}

time_t ltntstools_pid_stats_pid_get_last_update(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	return pid->pps_last_update;
}

uint64_t ltntstools_pid_stats_stream_get_scrambled_count(struct ltntstools_stream_statistics_s *stream)
{
	return stream->scrambledCount;
}

int ltntstools_pid_stats_stream_did_violate_pcr_timing(struct ltntstools_stream_statistics_s *stream)
{
	/* If the last _write cause the pcr's to be violated, exceeding 40ms, it's not always great. */
	return stream->prev_pcrExceeds40ms != stream->pcrExceeds40ms;
}

int ltntstools_pid_stats_pid_did_violate_pcr_timing(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr)
{
	/* If the last _write cause the pcr's to be violated, exceeding 40ms, it's not always great. */

	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	return pid->prev_pcrExceeds40ms != pid->pcrExceeds40ms;
}

int ltntstools_pid_stats_pid_get_pcr_walltime_driftms(struct ltntstools_stream_statistics_s *stream, uint16_t pidnr, int64_t *driftMs)
{
	struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr & 0x1fff];
	if (!pid->hasPCR) {
		return -1; /* Failed, not a PCR pid */
	}

	*driftMs = pid->lastPCRWalltimeDriftMs;

	return 0; /* Success */
}

void ltntstools_pid_stats_dprintf(struct ltntstools_stream_statistics_s *stream, int fd)
{
	dprintf(fd, "----------PID ---------Pkts -----CCErrors --Mbps\n");

	for (int i = 0; i < MAX_PID; i++) {
		if (!stream->pids[i].enabled)
			continue;

		dprintf(fd, "0x%04x (%4d) %13" PRIu64 " %13" PRIu64 " %6.02f\n",
			i,
			i,
			stream->pids[i].packetCount,
			stream->pids[i].ccErrors,
			stream->pids[i].mbps);
	}
}

uint32_t ltntstools_cc_reorder_table_readpos(struct ltntstools_cc_reorder_table_s *t, int offset)
{
        return ((t->writeIdx + 12) + offset) % LTNTSTOOLS_CC_REORDER_LIST_SIZE;
}

void ltntstools_cc_reorder_table_add(struct ltntstools_cc_reorder_table_s *t, uint16_t pid, uint8_t cc, int isCCError)
{
        if (t->updateCount < LTNTSTOOLS_CC_REORDER_LIST_SIZE) {
                t->updateCount++;
        }

        t->arr[t->writeIdx] = cc;
        t->ccerror[t->writeIdx] = isCCError;
        t->writeIdx = (t->writeIdx + 1) % LTNTSTOOLS_CC_REORDER_LIST_SIZE;
}

void ltntstools_cc_reorder_table_sum(struct ltntstools_cc_reorder_table_s *t, uint32_t *value, uint32_t *ccerrors)
{
        uint32_t v = 0;
		uint32_t cc = 0;
        for (int i = 0; i < LTNTSTOOLS_CC_REORDER_TRACKING_COUNT; i++) {
                uint32_t pos = ltntstools_cc_reorder_table_readpos(t, i);
                v += t->arr[pos];
                cc += t->ccerror[pos];
        }
		*value = v;
		*ccerrors = cc; 
}

int ltntstools_cc_reorder_table_corelate(struct ltntstools_cc_reorder_table_s *t)
{
	uint32_t total = 0, ccerrors = 0;
	int oopdetected_as_ccerrors = 0;
	ltntstools_cc_reorder_table_sum(t, &total, &ccerrors);
	if (total == 120 && ccerrors) {
		/* The sum of 0..f is 120 decimal */
		/* If the table shows a sum of 120 but has ccerrors, these were out of order packets */
		oopdetected_as_ccerrors = 1;
	}

	return oopdetected_as_ccerrors;
}

void ltntstools_cc_reorder_table_reset(struct ltntstools_cc_reorder_table_s *t)
{
	memset(t, 0, sizeof(*t));
}

void ltntstools_cc_reorder_table_print(struct ltntstools_cc_reorder_table_s *t)
{
		uint32_t total = 0, ccerrors = 0;
        ltntstools_cc_reorder_table_sum(t, &total, &ccerrors);

		int oop_detected = ltntstools_cc_reorder_table_corelate(t);

        printf("reordertable: writeIdx = %02d, updateCount %d, rb %02d re %02d, total %3d, OOP detected as CC %s\n",
                t->writeIdx, t->updateCount,
				ltntstools_cc_reorder_table_readpos(t, 0),
				ltntstools_cc_reorder_table_readpos(t, LTNTSTOOLS_CC_REORDER_TRACKING_COUNT - 1),
				total,
				oop_detected ? "true" : "false");
        for (int i = 0; i < LTNTSTOOLS_CC_REORDER_LIST_SIZE; i++) {
                printf("reordertable: arr[%2d] 0x%02x %02d, error %d\n", i, t->arr[i], t->arr[i], t->ccerror[i]);
        }
}

uint64_t ltntstools_pid_stats_stream_get_notmultipleofseven_errors(struct ltntstools_stream_statistics_s *stream)
{
	return stream->notMultipleOfSevenError;
}

time_t ltntstools_pid_stats_stream_get_notmultipleofseven_time(struct ltntstools_stream_statistics_s *stream)
{
	return stream->last_notMultipleOfSeven_error;
}

uint64_t ltntstools_pid_stats_stream_get_iat_hwm_us(struct ltntstools_stream_statistics_s *stream)
{
	return stream->iat_hwm_us_last_nsecond;
}
