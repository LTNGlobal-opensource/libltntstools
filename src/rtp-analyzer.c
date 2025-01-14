#include "libltntstools/ts.h"
#include "libltntstools/rtp-analyzer.h"
#include <arpa/inet.h>

void rtp_analyzer_init(struct rtp_hdr_analyzer_s *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
    ltn_histogram_alloc_video_defaults(&ctx->tsArrival, "RTP Header - Inter-RTP-Frame Arrival Times - IAT (ms)");
    ltn_histogram_alloc_video_defaults(&ctx->tsInterval, "RTP Header - Clock/Timestamp Intervals 90KHz (ms)");

}

void rtp_analyzer_free(struct rtp_hdr_analyzer_s *ctx)
{
	if (ctx->tsArrival) {
		ltn_histogram_free(ctx->tsArrival);
		ctx->tsArrival = NULL;
	}

	if (ctx->tsInterval) {
		ltn_histogram_free(ctx->tsInterval);
		ctx->tsInterval = NULL;
	}
}

int rtp_hdr_is_payload_type_valid(const struct rtp_hdr *hdr)
{
// TODO
	/* SMPTE 2110-10: "All RTP streams shall use dynamic payload types chosen in the
	 * range of 96 through 127, signaled as specified in section 6 of IETF RFC 4566,
	 * unless a fixed payload type designation exists for that RTP stream within the
	 * IETF standard which specifies it."
	*/
	if (hdr->pt < 96 || hdr->pt > 127) {
		if (hdr->pt != 33) {
			return 0;
		}
	}

	return 1;
}

int rtp_hdr_is_continious(struct rtp_hdr_analyzer_s *ctx, const struct rtp_hdr *hdr)
{
	int match = 1;

	if (ctx->last.seq > 0) {
		int next = (ntohs(ctx->last.seq) + 1) % 65536;
		if (next != ntohs(hdr->seq)) {
			match = 0;
		}
	}

	ctx->last = *hdr;

	return match;
}

int rtp_hdr_write(struct rtp_hdr_analyzer_s *ctx, const struct rtp_hdr *hdr)
{
	ctx->totalPackets++;

	struct timeval now;
	gettimeofday(&now, NULL);
    ltn_histogram_interval_update(ctx->tsArrival, &now);

	/* Push a clock measurement between old and new TS into a histogram */
	if (ctx->last.ts) {
		/* Convert 90KHz clock into ms and histogram it. */
		int64_t ticks = ltntstools_pts_diff(ntohl(ctx->last.ts), ntohl(hdr->ts)) / 90;
		ltn_histogram_interval_update_with_value(ctx->tsInterval, ticks);

		if (ticks == 0) {
			ctx->illegalTSTimestampStallEvents++;
		} else
		if (ticks >= 3) {
			ctx->illegalTSTimestampMovementEvents++;
		}
	}

	/* If the timestamp has moved between ftp frames, but the last frame
	 * didn't have the M bit set (end of frame), the timestamp moved illegally.
	 */
	if (ctx->last.ts && (ctx->last.ts != hdr->ts) && ctx->last.m == 0) {
		if (hdr->pt >= 96 && hdr->pt <= 127) {
			/* Only valid for 2110 streams */
			ctx->illegal2110TimestampMovementEvents++;
		}
	}

	/* If the timestamp NOT moved between rtp frames, and the last frame
	 * had the M bit set (end of frame), the timestamp stalled illegally.
	 */
	if (ctx->last.ts && (ctx->last.ts == hdr->ts) && ctx->last.m == 1) {
		ctx->illegal2110TimestampStallEvents++;
	}

	/* Is the 16 bit sequence number where we expect is to be? */
	if (rtp_hdr_is_continious(ctx, hdr) == 0)
		ctx->discontinuityEvents++;

	/* If the sequence moved backwards compared to prior seq */
	if (ctx->last.ts && (ntohs(hdr->seq) && (ntohs(hdr->seq) < ntohs(ctx->last.seq)))) {
		ctx->illegalTSCounterMovementEvents++;
	}

	if (rtp_hdr_is_payload_type_valid(hdr) == 0)
		ctx->illegalPayloadTypeEvents++;

	/* Count new frames as they arrive. */
	if (hdr->m)
		ctx->totalFrames++;

	return 0; /* Success */
}

void rtp_analyzer_reset(struct rtp_hdr_analyzer_s *ctx)
{
	ctx->totalPackets = 0;
	ctx->totalFrames = 0;
	ctx->discontinuityEvents = 0;
	ctx->illegalPayloadTypeEvents = 0;
	ctx->illegal2110TimestampMovementEvents = 0;
	ctx->illegal2110TimestampStallEvents = 0;
	ctx->illegalTSTimestampMovementEvents = 0;
	ctx->illegalTSTimestampStallEvents = 0;
	ctx->illegalTSCounterMovementEvents = 0;
	ltn_histogram_reset(ctx->tsArrival);
	ltn_histogram_reset(ctx->tsInterval);
}

void rtp_analyzer_report_dprintf(struct rtp_hdr_analyzer_s *ctx, int fd)
{
	dprintf(fd, "RTP Analyzer Report:\n");
	dprintf(fd, "\tTotal RTP packets = %" PRIi64 "\n", ctx->totalPackets);
	//dprintf(fd, "\tTotal TS frames = %" PRIi64 "\n", ctx->totalFrames);
	dprintf(fd, "\tTotal sequence counter discontinuity events = %" PRIi64 "\n", ctx->discontinuityEvents);
	dprintf(fd, "\tIllegal payload type events = %" PRIi64 "\n", ctx->illegalPayloadTypeEvents);
	dprintf(fd, "\tIllegal SMPTE2110 timestamp movement events = %" PRIi64 "\n", ctx->illegal2110TimestampMovementEvents);
	dprintf(fd, "\tIllegal SMPTE2110 timestamp stall events = %" PRIi64 "\n", ctx->illegal2110TimestampStallEvents);
	dprintf(fd, "\tIllegal TS timestamp movement events = %" PRIi64 "\n", ctx->illegalTSTimestampMovementEvents);
	dprintf(fd, "\tIllegal TS timestamp stall events = %" PRIi64 "\n", ctx->illegalTSTimestampStallEvents);
	dprintf(fd, "\tIllegal TS sequence movement events = %" PRIi64 "\n", ctx->illegalTSCounterMovementEvents);

	dprintf(fd, "\n");
    ltn_histogram_interval_print(fd, ctx->tsInterval, 0);
	dprintf(fd, "\n");
    ltn_histogram_interval_print(fd, ctx->tsArrival, 0);
}

void rtp_analyzer_hdr_dprintf(const struct rtp_hdr *h, int fd)
{
	dprintf(fd, "seq:%05d ts:%010u version:%d p:%d x:%d cc:%02d m:%d pt:%02d ssrc:0x%08x\n",
		ntohs(h->seq), ntohl(h->ts), h->version, h->p, h->x, h->cc, h->m, h->pt, ntohl(h->ssrc));
}

/* Find all of the rtp headers in a buffer
 * assertain all of the frame length by measuring the byte space between headers.
 * for the last RTP header, becasue we can't asertain it's length, leave that as a zero value.
*/
int rtp_frame_queryPositions(const unsigned char *buf, int lengthBytes, uint64_t addr,  uint32_t ssrc, struct rtp_frame_position_s **array, int *arrayLength)
{
	struct rtp_frame_position_s *arr = NULL;
	int arrLength = 0;

	/* The detector requires a minimum of two TS packets per RTP frame.
	 * This is an attempt to discard false positive finds
	 */
	for (int i = 0; i < lengthBytes - (2 * 188); i++) {

		const unsigned char *p = &buf[i];

		struct rtp_hdr *hdr = (struct rtp_hdr *)&buf[i];

		if (hdr->version != 2)
			continue; /* Not the correct RTP version, we need to start with a rtp header */

		if (hdr->pt != 33) { /* Discard anything not MPEGTS */
			continue;
		}

		if (hdr->x == 1) { /* Discard anything with extension bits */
			continue;
		}

		if (hdr->cc != 0) { /* Discard anything with Multiple CSRC identifiers */
			continue;
		}

		if (hdr->ssrc != ssrc) { /* Discard anything with Synchronization source identifier uniquely identifies the source of a stream, !+ 0 */
			continue;
		}

		if (*(p + 12) != 0x47)
			continue; /* And we didn't find a MPEG-TS sync byte, probably mis-detected the RTP header */

		if (*(p + 12 + 188) != 0x47) {
			/* Missing a second sync byte.
			* Try to find a second RTP header in its place (IE, we have a one ts packet RTP header in our buffer) */
			if ((*(p + 12 + 188) >> 6) != 2) {
				continue; /* And we didn't find a second MPEG-TS sync byte, probably mis-detected the RTP header */
			}
			/* Got another RPT header we think, final check, that this second header contains a SYNC */
			if (*(p + 12 + 188 + 12) != 0x47) {
				continue; /* We thought it was a second RTP header but it didn't contain a packet. Be safe, skip it*/
			}
		}
#if 0
		printf("Found frame at offset %6d pt %d: ", i, hdr->pt);
		for (int j = 0; j < 32; j++) {
			printf("%02x ", buf[i + j]);
		}
		printf("\n");
#endif
		arr = realloc(arr, ++arrLength * sizeof(struct rtp_frame_position_s));
		if (!arr)
			return -1;

		(arr + (arrLength - 1))->offset = addr + i;
		(arr + (arrLength - 1))->frame = *hdr; /* Copy the entire struct */
		(arr + (arrLength - 1))->lengthBytes = 0;

		/* Update the prior length, based on our current offset minus the prior offset */
		if (arrLength >= 2) {
			(arr + (arrLength - 2))->lengthBytes = (addr + i) - (arr + (arrLength - 2))->offset;
		}

		i += ((12 + 188) - 1); /* Optimize the next search */
	}

	*array = arr;
	*arrayLength = arrLength;

	return 0; /* Success */
}
