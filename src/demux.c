#include "demux-types.h"

#define LOCAL_DEBUG 0

void demux_pid_set_estype(struct demux_pid_s *pid, enum payload_e estype)
{
	pid->payload = estype;
}

void ltntstools_demux_free(void *hdl)
{
	struct demux_ctx_s *ctx = (struct demux_ctx_s *)hdl;

	ltntstools_pat_free((struct ltntstools_pat_s *)ctx->pat);

	for (int i = 0; i < MAX_PIDS;i++) {
		struct demux_pid_s *pid = _getPID(ctx, i);
		if (pid) {
			demux_pid_uninit(pid);
		}
	}
	free(ctx);
}

int ltntstools_demux_alloc_from_pat(void **hdl, void *userContext, const struct ltntstools_pat_s *pat)
{
	if (!pat) {
		return -1;
	}

	struct demux_ctx_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -1;
	}

	ctx->userContext = userContext;
	ctx->pat = ltntstools_pat_clone((struct ltntstools_pat_s *)pat);
	ctx->verbose = 0;

	for (int i = 0; i < MAX_PIDS;i++) {
		struct demux_pid_s *pid = _getPID(ctx, i);
		if (pid) {
			demux_pid_init(pid, i);
		}
	}

	/* Now walk the PAT and allocate any needed PES extractors */
	for (int p = 0; p < ctx->pat->program_count; p++) {
		const struct ltntstools_pat_program_s *program = &pat->programs[p];
		if (program->program_number == 0) {
			/* NIT */
			continue;
		}

		const struct ltntstools_pmt_s *pmt = &program->pmt;
		for (int s = 0; s < pmt->stream_count; s++) {
			const struct ltntstools_pmt_entry_s *stream = &pmt->streams[s];
			struct demux_pid_s *pid = _getPID(ctx, stream->elementary_PID);

			int detectedAudioDescriptor = 0;
			int detectedVideoDescriptor = 0;

			/* TODO: No support for SCTE35, SMPTE2038 or other private streams currently */

			if (stream->stream_type == 0x06 /* Private */) {
				if (ltntstools_descriptor_list_contains_smpte2064_registration((struct ltntstools_descriptor_list_s *)&stream->descr_list)) {
					/* Found a SMPTE-2064 stream. */
					if (ltntstools_pes_extractor_alloc(&pid->pe, stream->elementary_PID,
						0xbf, /* See SMPTE ST 2064-2 2015 section 7.2.1 */
						(pes_extractor_callback)demux_pid_pe_callback, pid, -1, -1) < 0);
					{
						fprintf(stderr, MODULE_PREFIX "Unable to allocate smpte2064 PE extractor for pid 0x%04x, skipping\n", stream->elementary_PID);
					}
					demux_pid_set_estype(pid, P_SMPTE2064);
				}
			} else
			if (detectedAudioDescriptor || ltntstools_pmt_entry_is_audio(stream)) {
				/* Audio Stream */
				/* TODO: convert the type to a stream ID */
				uint8_t streamId = 0xc0;

				if (ltntstools_pes_extractor_alloc(&pid->pe,
					stream->elementary_PID, streamId,
					(pes_extractor_callback)demux_pid_pe_callback, pid,	-1, -1) < 0);
				{
					fprintf(stderr, MODULE_PREFIX "Unable to allocate audio PE extractor for pid 0x%04x, skipping\n", stream->elementary_PID);
				}
				demux_pid_set_estype(pid, P_AUDIO);
			} else
			if (detectedVideoDescriptor || ltntstools_is_ESPayloadType_Video(stream->stream_type)) {
				/* Video Stream */
				/* TODO: convert the type to a stream ID */
				uint8_t streamId = 0xe0;

				if (ltntstools_pes_extractor_alloc(&pid->pe, stream->elementary_PID,
					streamId, (pes_extractor_callback)demux_pid_pe_callback, pid, -1, -1) < 0);
				{
					fprintf(stderr, MODULE_PREFIX "Unable to allocate video PE extractor for pid 0x%04x, skipping\n", stream->elementary_PID);
				}
				demux_pid_set_estype(pid, P_VIDEO);
			}
		}

	}

	*hdl = ctx;
	return 0; /* Success */
}

ssize_t ltntstools_demux_write(void *hdl, const uint8_t *pkts, uint32_t packetCount)
{
	struct demux_ctx_s *ctx = (struct demux_ctx_s *)hdl;
	if (!ctx || !pkts || !packetCount) {
		return -1;
	}

	for (int i = 0; i < packetCount; i++) {
		const uint8_t *pkt = &pkts[i];

		uint16_t pidNr = ltntstools_pid(pkt);

		struct demux_pid_s *pid = _getPID(ctx, pidNr);

		/* Update any pes extractors */
		if (pid->pe) {
			ltntstools_pes_extractor_write(pid->pe, pkt, 1);
		}
	}

	return packetCount;
}
