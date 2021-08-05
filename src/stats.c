/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include "libltntstools/ltntstools.h"

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

	/* Pull the CC out of the frame and check for CC loss. */
	uint16_t sequence_number = *(buf + 2) << 8 | *(buf + 3);
	if (((stream->a324_sequence_number + 1) & 0xffff) != sequence_number) {
		/* No CC error for the first packet. */
		if (stream->packetCount) {
			stream->ccErrors++;
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

void ltntstools_pid_stats_update(struct ltntstools_stream_statistics_s *stream, const uint8_t *pkts, uint32_t packetCount)
{
	time_t now;
	time(&now);

	for (int i = 0; i < packetCount; i++) {
		int offset = i * 188;
		if (*(pkts + offset) == 0x47)
			stream->packetCount++;
		else
			stream->ccErrors++;
	}

	if (now != stream->pps_last_update) {
		stream->pps = stream->pps_window;
		stream->pps_window = 0;
		stream->mbps = stream->pps;
		stream->mbps *= (188 * 8);
		stream->mbps /= 1e6;
		stream->pps_last_update = now;
	}
	stream->pps_window += packetCount;

	for (int i = 0; i < packetCount; i++) {
		int offset = i * 188;

		uint16_t pidnr = ltntstools_pid(pkts + offset);
		struct ltntstools_pid_statistics_s *pid = &stream->pids[pidnr];

		pid->enabled = 1;
		pid->packetCount++;

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
		if (ltntstools_isCCInError(pkts + offset, pid->lastCC)) {
			if (pid->packetCount > 1 && pidnr != 0x1fff) {
				pid->ccErrors++;
				stream->ccErrors++;
			}
		}

		pid->lastCC = cc;

		if (ltntstools_tei_set(pkts + offset)) {
			pid->teiErrors++;
			stream->teiErrors++;
		}
	}
}

void ltntstools_pid_stats_reset(struct ltntstools_stream_statistics_s *stream)
{
	stream->packetCount = 0;
	stream->teiErrors = 0;
	stream->ccErrors = 0;
	stream->mbps = 0;

	for (int i = 0; i < MAX_PID; i++) {
		if (!stream->pids[i].enabled)
			continue;
		stream->pids[i].packetCount = 0;
		stream->pids[i].ccErrors = 0;
		stream->pids[i].teiErrors = 0;
		stream->pids[i].mbps = 0;
	}
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

uint64_t ltntstools_pid_stats_stream_get_cc_errors(struct ltntstools_stream_statistics_s *stream)
{
	return stream->ccErrors;
}

