#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>
#include <dvbpsi/dvbpsi.h>
#include <dvbpsi/psi.h>
#include <dvbpsi/pat.h>

#include "libltntstools/ltntstools.h"

#define LOCAL_DEBUG 1

struct ltntstools_pat_parser_s
{
	dvbpsi_t *p_dvbpsi;

	void *userContext;

	//dvbpsi_pat_t *p_pat;
};

static void message(dvbpsi_t *handle, const dvbpsi_msg_level_t level, const char* msg)
{
	switch(level) {
	case DVBPSI_MSG_ERROR: fprintf(stderr, "Error: "); break;
	case DVBPSI_MSG_WARN:  fprintf(stderr, "Warning: "); break;
	case DVBPSI_MSG_DEBUG: fprintf(stderr, "Debug: "); break;
	default: /* do nothing */
		return;
	}
	fprintf(stderr, "%s\n", msg);
}

static void cb_pat(void *p_zero, dvbpsi_pat_t *p_pat)
{
	struct ltntstools_pat_parser_s *ctx = p_zero;
#if LOCAL_DEBUG
	printf("%s(%p)\n", __func__, ctx);
#endif

	dvbpsi_pat_program_t* p_program = p_pat->p_first_program;

	printf("\n");
	printf("PAT\n");
	printf("  transport_stream_id : 0x%x\n", p_pat->i_ts_id);
	printf("  version_number      : 0x%x\n", p_pat->i_version);
	printf("    | program_number @ PID\n");

	while(p_program) {
		printf("    | %14d @ 0x%x (%d)\n", p_program->i_number, p_program->i_pid, p_program->i_pid);
		p_program = p_program->p_next;
	}
	printf(  "  active              : %d\n", p_pat->b_current_next);

	dvbpsi_pat_delete(p_pat);
}

void ltntstools_pat_parser_free(void *hdl)
{
	struct ltntstools_pat_parser_s *ctx = hdl;
#if LOCAL_DEBUG
	printf("%s(%p)\n", __func__, ctx);
#endif
	if (!ctx)
		return;

	free(ctx);
}

#if 0
void ltntstools_pat_parser_section_delete(void *hdl, dvbpsi_pat_t *p_pat)
{
	dvbpsi_pat_delete(p_pat);
}
#endif

int ltntstools_pat_parser_write(void *hdl, const unsigned char *pkt, int packetCount, int *complete)
{
	struct ltntstools_pat_parser_s *ctx = hdl;

	for (int i = 0; i < packetCount; i++) {
		if (ltntstools_pid(&pkt[i * 188]) == 0)
			dvbpsi_packet_push(ctx->p_dvbpsi, (unsigned char *)&pkt[i * 188]);
	}

	return 0;
}

int ltntstools_pat_parser_alloc(void **hdl, void *userContext)
{
#if LOCAL_DEBUG
	printf("%s(%p)\n", __func__, userContext);
#endif
	struct ltntstools_pat_parser_s *ctx = calloc(1, sizeof(*ctx));
	ctx->userContext = userContext;
	ctx->p_dvbpsi = dvbpsi_new(&message, DVBPSI_MSG_DEBUG);

	if (!dvbpsi_pat_attach(ctx->p_dvbpsi, cb_pat, ctx)) {
		ltntstools_pat_parser_free(ctx);
		return -1;
	}

	*hdl = ctx;
	return 0;
}

struct ltntstools_pat_s *ltntstools_pat_alloc()
{
	struct ltntstools_pat_s *pat = calloc(1, sizeof(*pat));

	return pat;
}

void ltntstools_pat_free(struct ltntstools_pat_s *pat)
{
	free(pat);
}

void ltntstools_pat_dprintf(struct ltntstools_pat_s *pat, int fd)
{
	dprintf(fd, "PAT\n");
	dprintf(fd, "\ttransport_stream_id = 0x%x\n", pat->transport_stream_id);
	dprintf(fd, "\tversion = 0x%x\n", pat->version);
	dprintf(fd, "\tcurrent_next = 0x%x\n", pat->current_next);
	dprintf(fd, "\tprogram_count = %d\n", pat->program_count);
	for (int i = 0; i < pat->program_count; i++) {
		dprintf(fd, "\t\tprogram_number = %d, pid 0x%04x\n",
			pat->programs[i].program_number,
			pat->programs[i].pid);
	}
}
