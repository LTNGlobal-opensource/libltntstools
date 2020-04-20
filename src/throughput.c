/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include "libltntstools/ltntstools.h"

void ltntstools_throughput_write_value(struct ltntstools_throughput_s *stream, int value)
{
	time_t now;
	time(&now);

	if (now != stream->Bps_last_update) {
		stream->Bps = stream->Bps_window;
		stream->Bps_window = 0;
		stream->mbps = stream->Bps;
		stream->mbps /= 1e6;
		stream->Bps_last_update = now;
	}
	stream->Bps_window += value;
}

void ltntstools_throughput_write_bytes(struct ltntstools_throughput_s *stream, const uint8_t *buf, uint32_t byteCount)
{
	time_t now;
	time(&now);

	if (now != stream->Bps_last_update) {
		stream->Bps = stream->Bps_window;
		stream->Bps_window = 0;
		stream->mbps = stream->Bps;
		stream->mbps *= 8;
		stream->mbps /= 1e6;
		stream->Bps_last_update = now;
	}
	stream->Bps_window += byteCount;
}

void ltntstools_throughput_reset(struct ltntstools_throughput_s *stream)
{
	stream->byteCount = 0;
	stream->mbps = 0;
	stream->mBps = 0;
}

static void _expire_per_second_stats(struct ltntstools_throughput_s *stream)
{
	time_t now;
	time(&now);

	if (now > stream->Bps_last_update + 2) {
		stream->mbps = 0;
		stream->mBps = 0;
		stream->Bps = 0;
		stream->Bps_window = 0;
	}
}

double ltntstools_throughput_get_mbps(struct ltntstools_throughput_s *stream)
{
	_expire_per_second_stats(stream);
	return stream->mbps;
}

uint32_t ltntstools_throughput_get_bps(struct ltntstools_throughput_s *stream)
{
	_expire_per_second_stats(stream);
	return stream->Bps * 8;
}

uint32_t ltntstools_throughput_get_value(struct ltntstools_throughput_s *stream)
{
	_expire_per_second_stats(stream);
	return stream->Bps;
}

