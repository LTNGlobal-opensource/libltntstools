#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <pthread.h>
#include <inttypes.h>

#include <stdbool.h>
#include <dvbpsi/dvbpsi.h>
#include <dvbpsi/psi.h>
#include <dvbpsi/descriptor.h>
#include <dvbpsi/pat.h>
#include <dvbpsi/pmt.h>
#include <dvbpsi/dr.h>

#include "libltntstools/streammodel.h"
#include "libltntstools/ts.h"
#include "libltntstools/pat.h"

static void message(dvbpsi_t *handle, const dvbpsi_msg_level_t level, const char* msg);

/* Running Object Model: A model of an entire ISO13818 stream,
 * Including PAT/PMT configurations, PIDS being used, when and how.
 * Caveats:
 *   Descriptors not supported.
 *   This isn't a statistical collection process, its a PAT/PMT and other
 *   suite of parsers enabling higher level applications to quickly understand
 *   the structure of any given stream.
 */
struct streammodel_pid_s
{
	int pid;
	int present;	/* Boolean: Are packets for this pid present in the stream */

	enum {
		PT_UNKNOWN = 0,
		PT_PAT,
		PT_PMT,
		PT_ES, /* Elementary stream. */
	} pidType;

	/* Elementary Stream details */
	//int estype;

	/* DVBPSI Cached Data */
	dvbpsi_t *p_dvbpsi;
	dvbpsi_pat_t *p_pat;
	dvbpsi_pmt_t *p_pmt;

	/* Housekeeping */
	struct streammodel_rom_s *rom;
	uint64_t packetCount;
	struct timeval lastUpdate;
};

/* No pointers allowed in here.... */
struct streammodel_rom_s
{
	int nr;
	int totalPMTsInPAT;	/* Total number of program stream PMT entries in the pat. */
	int parsedPMTs;		/* Total number of PMT callbacks we've processed, we need them all to finish the model. */
	int modelComplete;	/* Boolean. */

	struct timeval allowableWriteTime;

#define MAX_ROM_PIDS 0x2000
	struct streammodel_pid_s pids[MAX_ROM_PIDS];

	/* Housekeeping */
	struct streammodel_ctx_s *ctx;
};

struct streammodel_ctx_s
{
	/* The framework builds two working models of the incoming stream, A and B.
	 * The stream is assumed to change every 500ms, so we constantly build a current model.
	 * 'current' points to the last known good model. 
	 * into 'current' when its considered complete and its safe to switch pointers.
	 */
	pthread_mutex_t rom_mutex;
	struct streammodel_rom_s *current;	/* Model any user queries will run against. */
	struct streammodel_rom_s *next;		/* Model currently being build by the framework, never user accessible. */
	struct streammodel_rom_s roms[2];	/* Storage for the models. */

	/* Housekeeping */
	struct timeval now;			/* Each write() call updates this */
};

/* ROM */
static void _rom_next_complete(struct streammodel_ctx_s *ctx)
{
	ctx->next->modelComplete = 1;
}

static void _rom_initialize(struct streammodel_ctx_s *ctx, struct streammodel_rom_s *rom, int nr)
{
	for (int i = 0; i < MAX_ROM_PIDS; i++) {
		struct streammodel_pid_s *ps = &rom->pids[i];

		/* Free any pid allocations. */
		ps->present = 0;
		ps->pid = i;
		ps->rom = rom;

		if (ps->p_pat) {
			dvbpsi_pat_delete(ps->p_pat);
			ps->p_pat = NULL;
			dvbpsi_pat_detach(ps->p_dvbpsi);
			ps->p_dvbpsi = NULL;
		}
		if (ps->p_pmt) {
			dvbpsi_pmt_delete(ps->p_pmt);
			ps->p_pmt = NULL;
			dvbpsi_pmt_detach(ps->p_dvbpsi);
			ps->p_dvbpsi = NULL;
		}

		if (ps->p_dvbpsi) {
			dvbpsi_delete(ps->p_dvbpsi);
			ps->p_dvbpsi = NULL;
		}

		/* Everything else */
		ps->packetCount = 0;
	}
	rom->nr = nr;
	rom->ctx = ctx;
	rom->modelComplete = 0;
	rom->parsedPMTs = 0;
	rom->totalPMTsInPAT = 0;
}

void _rom_activate(struct streammodel_ctx_s *ctx)
{
	/* Promote the 'next' rom */
	struct streammodel_rom_s *c = ctx->current;

	ctx->current = ctx->next;

	ctx->next = c;

	/* Re-initialize what was current. */
	_rom_initialize(ctx, ctx->next, ctx->next->nr);

	/* Don't start writing packets into the next model for 1 second. */
	struct timeval future = { 0, 500 * 1000 };
	timeradd(&future, &ctx->now, &ctx->next->allowableWriteTime);
}

static struct streammodel_pid_s *_rom_next_find_pid(struct streammodel_ctx_s *ctx, uint16_t pid)
{
	return &ctx->next->pids[pid];
}

/* End: ROM */

/* DVBPSI */
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

static void cb_pmt(void *p_zero, dvbpsi_pmt_t *p_pmt)
{
	struct streammodel_pid_s *ps = p_zero;
	struct streammodel_rom_s *rom = ps->rom;
	struct streammodel_ctx_s *ctx = rom->ctx;

	dvbpsi_pmt_es_t* p_es = p_pmt->p_first_es;

	printf("New active PMT (%d)\n", ps->pid);
	printf("  program_number : %d\n", p_pmt->i_program_number);

	while(p_es) {
		printf("    pid %04x estype %02x\n", p_es->i_pid, p_es->i_type);

		struct streammodel_pid_s *es = _rom_next_find_pid(ctx, p_es->i_pid);
		es->present = 1;
		es->pidType = PT_ES;

		p_es = p_es->p_next;
	}

	/* Don't delete p_pmt, we're caching it. */
	ps->p_pmt = p_pmt;

	rom->parsedPMTs++;

	printf("%s() parsed %d expecting %d\n", __func__, rom->parsedPMTs, rom->totalPMTsInPAT);
	if (rom->parsedPMTs == rom->totalPMTsInPAT) {
		printf("Model#%d collection complete, %d PMTs collected\n", rom->nr, rom->parsedPMTs);
		_rom_next_complete(ctx);
		_rom_activate(ctx);
	}
}

static void cb_pat(void *p_zero, dvbpsi_pat_t *p_pat)
{
	struct streammodel_pid_s *ps = p_zero;
	struct streammodel_rom_s *rom = ps->rom;
	struct streammodel_ctx_s *ctx = rom->ctx;

	printf("%s(%p, pat %p) pid 0x%x model#%d\n", __func__, ps, p_pat, ps->pid, ps->rom->nr);

	dvbpsi_pat_program_t *p_program = p_pat->p_first_program;

	printf("\n");
	printf("PAT\n");
	printf("  transport_stream_id : 0x%x\n", p_pat->i_ts_id);
	printf("  version_number      : 0x%x\n", p_pat->i_version);
	printf("    | program_number @ PID\n");

	rom->totalPMTsInPAT = 0;

	while(p_program) {
		printf("    | %14d @ 0x%x (%d)\n", p_program->i_number, p_program->i_pid, p_program->i_pid);

		/* Build a new parser for the PMT. */
		struct streammodel_pid_s *m = _rom_next_find_pid(ctx, p_program->i_pid);

		m->present = 1;
		m->pidType = PT_PMT;

		m->p_dvbpsi = dvbpsi_new(&message, DVBPSI_MSG_DEBUG);
		if (!m->p_dvbpsi) {
			fprintf(stderr, "%s() PSI alloc. Should never happen\n", __func__);
			exit(1);
		}

		if (!dvbpsi_pmt_attach(m->p_dvbpsi, p_program->i_number, cb_pmt, m))
		{
			fprintf(stderr, "%s() PMT attach. Should never happen\n", __func__);
			exit(1);
		}

		rom->totalPMTsInPAT++;

		p_program = p_program->p_next;

	}
	printf(  "  active              : %d\n", p_pat->b_current_next);
	printf(  "  PMTS %d\n", rom->totalPMTsInPAT);

	/* Don't delete p_pat, we're caching it. */
	ps->p_pat = p_pat;
}
/* End: DVBPSI */

int ltntstools_streammodel_alloc(void **hdl)
{
	struct streammodel_ctx_s *ctx = calloc(1, sizeof(*ctx));

	ctx->current = calloc(1, sizeof(*ctx));
	pthread_mutex_init(&ctx->rom_mutex, NULL);

	_rom_initialize(ctx, &ctx->roms[0], 0);
	_rom_initialize(ctx, &ctx->roms[1], 1);
	ctx->current = &ctx->roms[0];
	ctx->next = &ctx->roms[1];

	_rom_activate(ctx);

	*hdl = ctx;
	return 0;
}

void ltntstools_streammodel_free(void *hdl)
{
	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;
	free(ctx);
}

/* pkt lists must be aligned. list may contant one or more packets. */
size_t ltntstools_streammodel_write(void *hdl, const unsigned char *pkt, int packetCount, int *complete)
{
	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;

	pthread_mutex_lock(&ctx->rom_mutex);

	gettimeofday(&ctx->now, NULL);

	int writePackets = 0;

	if (timercmp(&ctx->now, &ctx->next->allowableWriteTime, >=)) {
		writePackets = 1;
	}

	for (int i = 0; writePackets && i < packetCount; i++) {

		uint16_t pid = ltntstools_pid(&pkt[i * 188]);
		if (pid > 0x1fff) {
			/* Assume junk */
				continue;
		}

		/* Find the next pid struct for this pid */
		struct streammodel_pid_s *ps = _rom_next_find_pid(ctx, pid);

		/* Initialize the PAT parser if this is the first time around. */
		if (pid == 0 && ps->packetCount == 0) {
			ps->present = 1;
			ps->pidType = PT_PAT;

			ps->p_dvbpsi = dvbpsi_new(&message, DVBPSI_MSG_DEBUG);

			if (!dvbpsi_pat_attach(ps->p_dvbpsi, cb_pat, ps))
			{
				fprintf(stderr, "%s() PAT attach. Should never happen\n", __func__);
				exit(1);
			}
		}

		if (ps->present && ps->p_dvbpsi) {
			dvbpsi_packet_push(ps->p_dvbpsi, (unsigned char *)&pkt[i * 188]);
		}

		ps->packetCount++;
		ps->lastUpdate = ctx->now;
	}

	*complete = ctx->current->modelComplete;

	pthread_mutex_unlock(&ctx->rom_mutex);

	return packetCount;
}

static void _streammodel_dprintf(struct streammodel_ctx_s *ctx, int fd, struct streammodel_rom_s *rom)
{
	dprintf(fd, "%s() model#%d\n", __func__, rom->nr);
	for (int i = 0; i < MAX_ROM_PIDS; i++) {
		struct streammodel_pid_s *ps = &rom->pids[i];
		if (!ps->present)
			continue;

		dprintf(fd, "%04x %d %" PRIu64 "\n",
			ps->pid,
			ps->pidType,
			ps->packetCount);
	}
}

void ltntstools_streammodel_dprintf(void *hdl, int fd)
{
	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;

	pthread_mutex_lock(&ctx->rom_mutex);
	_streammodel_dprintf(ctx, fd, ctx->next);
	pthread_mutex_unlock(&ctx->rom_mutex);
}

int ltntstools_streammodel_query_model(void *hdl, struct ltntstools_pat_s **pat)
{
	int ret = 0;
printf("%s()\n", __func__);

	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;

	pthread_mutex_lock(&ctx->rom_mutex);
	if (ctx->current->modelComplete) {

//		_streammodel_dprintf(ctx, 0, ctx->current);

		struct streammodel_pid_s *ps = &ctx->current->pids[0];

		struct ltntstools_pat_s *p = ltntstools_pat_alloc();

		dvbpsi_pat_t *stream_pat = ps->p_pat;
		if (stream_pat) {

			/* Convert the dvbpsi struct into a new obj. */
			p->transport_stream_id = stream_pat->i_ts_id;
			p->version = stream_pat->i_version;
			p->current_next = stream_pat->b_current_next;

			dvbpsi_pat_program_t *e = stream_pat->p_first_program;
			while (e) {
				p->programs[p->program_count].program_number = e->i_number;
				p->programs[p->program_count].pid = e->i_pid;
				p->program_count++;
				e = e->p_next;
			}
			*pat = p;
		} else {
			ltntstools_pat_free(p);
			ret = -1;
		}


	} else {
		ret = -1;
	}
	pthread_mutex_unlock(&ctx->rom_mutex);

	return ret;
}

