#include "streammodel-types.h"

#if 0
#define DVBPSI_REPORTING (DVBPSI_MSG_DEBUG)
#else
#define DVBPSI_REPORTING (DVBPSI_MSG_ERROR)
#endif

extern void extractors_free(struct streammodel_ctx_s *ctx);

static void message(dvbpsi_t *handle, const dvbpsi_msg_level_t level, const char* msg);
static int _streammodel_query_model(struct streammodel_ctx_s *ctx, struct streammodel_rom_s *rom, struct ltntstools_pat_s **pat);

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
		}
		if (ps->pidType == PT_PAT && ps->parser[0].p_dvbpsi) {
			dvbpsi_pat_detach(ps->parser[0].p_dvbpsi);
		}

		for (int j = 0; j < MAX_PID_PARSERS ; j++) {
			if (ps->parser[j].p_pmt) {
				dvbpsi_pmt_delete(ps->parser[j].p_pmt);
				ps->parser[j].p_pmt = NULL;
			}
			if (ps->pidType == PT_PMT && ps->parser[j].p_dvbpsi) {
				dvbpsi_pmt_detach(ps->parser[j].p_dvbpsi);
			}
		}

		for (int j = 0; j < MAX_PID_PARSERS ; j++) {
			if (ps->parser[j].p_dvbpsi) {
				dvbpsi_delete(ps->parser[j].p_dvbpsi);
				ps->parser[j].p_dvbpsi = NULL;
			}
		}

		/* Everything else */
		ps->packetCount = 0;
	}
	rom->nr = nr;
	rom->ctx = ctx;
	rom->modelComplete = 0;
	rom->parsedPMTs = 0;
	rom->totalPMTsInPAT = 0;
	rom->pmtCollectionTimer.tv_sec = 0;
}

void _rom_activate(struct streammodel_ctx_s *ctx)
{
	/* Compare current and next models, bounce the version if
	 * we've detected a change.
	 */

	int ret;

	struct ltntstools_pat_s *patCurrent = NULL;
	struct ltntstools_pat_s *patNext = NULL;
	ret = _streammodel_query_model(ctx, ctx->current, &patCurrent);
	ret = _streammodel_query_model(ctx, ctx->next, &patNext);

	if (patCurrent && patNext) {
		ret = ltntstools_pat_compare(patCurrent, patNext);
		if (ret != 0) {
			ctx->currentModelVersion++;
			printf("*** NEW MODEL DETECTED as 0x%016" PRIx64 "***\n", ctx->currentModelVersion);
		} else {
#if 0
			printf("*** current/next models are identical, no changes detected ***\n");
#endif
		}
	}

	if (patCurrent) {
		ltntstools_pat_free(patCurrent);
		patCurrent = NULL;
	}
	if (patNext) {
		ltntstools_pat_free(patNext);
		patNext = NULL;
	}

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

static struct streammodel_pid_s *_rom_find_pid(struct streammodel_rom_s *rom, uint16_t pid)
{
	return &rom->pids[pid];
}

#if 0
static struct streammodel_pid_s *_rom_current_find_pid(struct streammodel_ctx_s *ctx, uint16_t pid)
{
	return _rom_find_pid(ctx->current, pid);
}
#endif

static struct streammodel_pid_s *_rom_next_find_pid(struct streammodel_ctx_s *ctx, uint16_t pid)
{
	return _rom_find_pid(ctx->next, pid);
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

#define CHATTY_CALLBACKS 0

static void cb_pmt(void *p_zero, dvbpsi_pmt_t *p_pmt)
{
	struct streammodel_pid_s *ps = p_zero;
	struct streammodel_rom_s *rom = ps->rom;
	struct streammodel_ctx_s *ctx = rom->ctx;

	dvbpsi_pmt_es_t* p_es = p_pmt->p_first_es;

#if CHATTY_CALLBACKS
	printf("New active PMT (%d)\n", ps->pid);
	printf("  program_number : %d\n", p_pmt->i_program_number);
#endif
	while(p_es) {
#if CHATTY_CALLBACKS
		printf("    pid 0x%04x estype %02x\n", p_es->i_pid, p_es->i_type);
#endif
		struct streammodel_pid_s *es = _rom_next_find_pid(ctx, p_es->i_pid);
		es->present = 1;
		es->pidType = PT_ES;

		p_es = p_es->p_next;
	}

	/* Find the appropriate PMT in the array matching this program number. */
	int idx = 0;
	for (int j = 0; j < MAX_PID_PARSERS;j++) {
		if (ps->parser[j].programNumber == p_pmt->i_program_number)
			idx = j;
	}
	/* Don't delete p_pmt, we're caching it. */
	ps->parser[idx].p_pmt = p_pmt;
	rom->parsedPMTs++;

#if CHATTY_CALLBACKS
	printf("%s() parsed %d expecting %d\n", __func__, rom->parsedPMTs, rom->totalPMTsInPAT);
#endif
	if (rom->parsedPMTs == rom->totalPMTsInPAT) {
#if CHATTY_CALLBACKS
		printf("Model#%d collection complete, %d PMTs collected\n", rom->nr, rom->parsedPMTs);
#endif
		_rom_next_complete(ctx);
//		_rom_activate(ctx);
	}
}

static void cb_pat(void *p_zero, dvbpsi_pat_t *p_pat)
{
	struct streammodel_pid_s *ps = p_zero;
	struct streammodel_rom_s *rom = ps->rom;
	struct streammodel_ctx_s *ctx = rom->ctx;

#if CHATTY_CALLBACKS
	printf("%s(%p, pat %p) pid 0x%x model#%d\n", __func__, ps, p_pat, ps->pid, ps->rom->nr);
#endif

	dvbpsi_pat_program_t *p_program = p_pat->p_first_program;

	if (rom->totalPMTsInPAT) {
		/* We might already have a PAT in progress. If we do, and another pat has
		 * arrived with a differnt version number, that means the stream was
		 * randomly changed underneat is, and we're not receiving
		 * a completely different stream.
		 * Abort the processing of the previous PAT, cleanup the rom and start again
		 * with the PAT.
		 */
		struct streammodel_pid_s *m = _rom_next_find_pid(ctx, 0);
		if (m->p_pat->i_version != p_pat->i_version && p_pat->b_current_next) {
#if CHATTY_CALLBACKS
			printf("New PAT arrived before the prior PAT complete version 0x%02x vs 0x%02x.\n",
				m->p_pat->i_version, p_pat->i_version);
#endif
			dvbpsi_pat_delete(p_pat);
			ctx->restartModel = 1;
			return;
		}
	}

#if CHATTY_CALLBACKS
	printf("\n");
	printf("PAT\n");
	printf("  transport_stream_id : 0x%04x\n", p_pat->i_ts_id);
	printf("  version_number      : 0x%04x\n", p_pat->i_version);
	printf("  current_next        : %d\n", p_pat->b_current_next);
	printf("    | program_number @ PID\n");
#endif

	/* Maximum of 1 second to gather PMT. */
	struct timeval future = { 1, 0 };
	timeradd(&future, &ctx->now, &ctx->next->pmtCollectionTimer);

	rom->totalPMTsInPAT = 0;

	while(p_program) {
#if CHATTY_CALLBACKS
		printf("    | %14d @ 0x%04x (%d)\n", p_program->i_number, p_program->i_pid, p_program->i_pid);
#endif

		/* Program# 0 is reserved for NIT tables. We don't expect a PMT for these. */
		if (p_program->i_number > 0) {

			if (ctx->enableSectionCRCChecks) {
				extractors_add(ctx, p_program->i_pid, 0x02 /* TableID */, "PMT", STREAMMODEL_CB_CONTEXT_PMT);
			}

			/* Build a new parser for the PMT. */
			struct streammodel_pid_s *m = _rom_next_find_pid(ctx, p_program->i_pid);

			m->present = 1;
			m->pidType = PT_PMT;

			int idx = 0;
			for (int i = 0; i < MAX_PID_PARSERS; i++) {
				if (m->parser[i].p_dvbpsi == 0) {
					idx = i;
					break;
				}
			}

			if (m->parser[idx].p_dvbpsi) {
				dvbpsi_pmt_detach(m->parser[idx].p_dvbpsi);
				dvbpsi_delete(m->parser[idx].p_dvbpsi);
				m->parser[idx].p_dvbpsi = NULL;
			}

			m->parser[idx].p_dvbpsi = dvbpsi_new(&message, DVBPSI_REPORTING);
			if (!m->parser[idx].p_dvbpsi) {
				fprintf(stderr, "%s() PSI alloc. Should never happen\n", __func__);
				exit(1);
			}

			if (!dvbpsi_pmt_attach(m->parser[idx].p_dvbpsi, p_program->i_number, cb_pmt, m))
			{
				fprintf(stderr, "%s() PMT attach. Should never happen\n", __func__);
				exit(1);
			}
			m->parser[idx].programNumber = p_program->i_number;

			rom->totalPMTsInPAT++;
		}

		p_program = p_program->p_next;

	}
#if CHATTY_CALLBACKS
	printf(  "  active              : %d\n", p_pat->b_current_next);
	printf(  "  PMTS %d\n", rom->totalPMTsInPAT);
#endif

	/* Don't delete p_pat, we're caching it. */
	ps->p_pat = p_pat;
}
/* End: DVBPSI */

int ltntstools_streammodel_alloc(void **hdl, void *userContext)
{
	struct streammodel_ctx_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	pthread_mutex_init(&ctx->rom_mutex, NULL);

	_rom_initialize(ctx, &ctx->roms[0], 0);
	_rom_initialize(ctx, &ctx->roms[1], 1);
	ctx->current = &ctx->roms[0];
	ctx->next = &ctx->roms[1];
	ctx->userContext = userContext;

	_rom_activate(ctx);

	*hdl = ctx;
	return 0;
}

void ltntstools_streammodel_free(void *hdl)
{
	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;

	/* Take the lock forever */
	pthread_mutex_lock(&ctx->rom_mutex);

	_rom_initialize(ctx, &ctx->roms[0], 0);
	_rom_initialize(ctx, &ctx->roms[1], 1);

	extractors_free(ctx);

	free(ctx);
}

/* pkt lists must be aligned. list may contant one or more packets. */
size_t ltntstools_streammodel_write(void *hdl, const unsigned char *pkt, int packetCount, int *complete)
{
	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;

	pthread_mutex_lock(&ctx->rom_mutex);

	gettimeofday(&ctx->now, NULL);

	ctx->writePackets = 0;

	if (timercmp(&ctx->now, &ctx->next->allowableWriteTime, >=)) {
		ctx->writePackets = 1;
	}
	if (ctx->next->pmtCollectionTimer.tv_sec && timercmp(&ctx->now, &ctx->next->pmtCollectionTimer, >=)) {
		/* We waited too long for all the PMTs to arrive, malformed
		 * stream or the stream PSI was changed underneath us.
		 */
		if (ctx->next->totalPMTsInPAT && ctx->next->parsedPMTs < ctx->next->totalPMTsInPAT) {
			printf("Stream PMT didn't arrive %d vs %d, aborting\n", ctx->next->totalPMTsInPAT, ctx->next->parsedPMTs);
			_rom_initialize(ctx, ctx->next, ctx->next->nr);
			ctx->writePackets = 1;
			//*complete = 0;
			ctx->current->modelComplete = 0;
		}
	}

#if 0
	if (!ctx->writePackets)
		printf("Not writing packets\n");
#endif

	if (ctx->enableSectionCRCChecks) {
		if (ctx->seCount == 0) {
			extractors_alloc(ctx);
		}
		extractors_write(ctx, pkt, packetCount);
	}

	for (int i = 0; ctx->writePackets && i < packetCount; i++) {

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

			ps->parser[0].p_dvbpsi = dvbpsi_new(&message, DVBPSI_REPORTING);

			if (!dvbpsi_pat_attach(ps->parser[0].p_dvbpsi, cb_pat, ps))
			{
				fprintf(stderr, "%s() PAT attach. Should never happen\n", __func__);
				exit(1);
			}

		}

		if (ps->present) {
			for (int j = 0; j < MAX_PID_PARSERS; j++) {
				if (ps->parser[j].p_dvbpsi == NULL)
					continue;
				dvbpsi_packet_push(ps->parser[j].p_dvbpsi, (unsigned char *)&pkt[i * 188]);
			}
		}

		ps->packetCount++;
		ps->lastUpdate = ctx->now;

		if (ctx->restartModel) {
			ctx->restartModel = 0;

			/* Re-initialize what was current. */
			_rom_initialize(ctx, ctx->next, ctx->next->nr);

			/* Don't start writing packets into the next model for N time period */
			struct timeval future = { 0, 500 * 1000 };
			timeradd(&future, &ctx->now, &ctx->next->allowableWriteTime);

			break;
		}
	}

	if (ctx->next->modelComplete) {
		_rom_activate(ctx);
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

static int _streammodel_query_model(struct streammodel_ctx_s *ctx, struct streammodel_rom_s *rom, struct ltntstools_pat_s **pat)
{
	int ret = 0;
//printf("%s() model#%d\n", __func__, rom->nr);

#if 0
	/* If no writes have occured to the stream in N seconds, any existing model is
	 * deemed invalid. ctx->now is the last time a write() call was made.
	 */
	struct timeval future = { 3, 0 };
	struct timeval modelTimeout;
	timeradd(&future, &ctx->now, &modelTimeout);

	struct timeval tod;
	gettimeofday(&tod, NULL);
	if (timercmp(&tod, &modelTimeout, >=)) {
		/* Invalidate the model. */
		printf("Invalidating the model\n");
		ctx->restartModel = 1;
		rom->modelComplete = 0;
	}
#endif
	if (rom->modelComplete) {

//		_streammodel_dprintf(ctx, 0, rom);

		struct streammodel_pid_s *ps = &rom->pids[0];

		dvbpsi_pat_t *stream_pat = ps->p_pat;
		if (stream_pat) {

			struct ltntstools_pat_s *newpat = ltntstools_pat_alloc_from_existing(stream_pat);

			/* For each pmt in the model, add this to our new object. */
			for (int i = 0; i < newpat->program_count; i++) {
				if (newpat->programs[i].program_number == 0)
					continue; /* Network PID */

				struct streammodel_pid_s *c = _rom_find_pid(rom, newpat->programs[i].program_map_PID);
				if (c->present == 0)
					continue;

				for (int j = 0; j < MAX_PID_PARSERS; j++) {
					if (c->parser[j].p_pmt && c->parser[j].programNumber ==
						newpat->programs[i].program_number)
					{
						ltntstools_pat_add_from_existing(newpat, c->parser[j].p_pmt);
					}
				}
				
			}

			*pat = newpat;

		} else {
			ret = -1;
		}


	} else {
		ret = -1;
	}

	return ret;
}

int ltntstools_streammodel_query_model(void *hdl, struct ltntstools_pat_s **pat)
{
	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;

	pthread_mutex_lock(&ctx->rom_mutex);
	int ret = _streammodel_query_model(ctx, ctx->current, pat);
	pthread_mutex_unlock(&ctx->rom_mutex);

	return ret;
}

int ltntstools_streammodel_is_model_mpts(void *hdl, struct ltntstools_pat_s *pat)
{
	int validServices = 0;
	for (int i = 0; i < pat->program_count; i++) {
		if (pat->programs[i].program_number == 0)
			continue;
		validServices++;
	}

	return validServices > 1 ? 1 : 0;
}

int ltntstools_streammodel_query_first_program_pcr_pid(void *hdl, struct ltntstools_pat_s *pat, uint16_t *PCRPID)
{
	if (!pat || !PCRPID)
		return -1;

	*PCRPID = 0;
	for (int i = 0; i < pat->program_count; i++) {
		if (pat->programs[i].program_number == 0)
			continue; /* Skip the network pid */

		if (pat->programs[i].pmt.stream_count == 0)
			continue; /* Skip programs with no streams */

		if (pat->programs[i].pmt.PCR_PID) {
			*PCRPID =  pat->programs[i].pmt.PCR_PID;
			return 0; /* Success */
		}
	}

	return -1; /* Failed */
}

int ltntstools_streammodel_enable_tr101290_section_checks(void *hdl, ltntstools_streammodel_callback cb)
{
	struct streammodel_ctx_s *ctx = (struct streammodel_ctx_s *)hdl;
	ctx->enableSectionCRCChecks = 1;
	ctx->cb = cb;

	return 0;
}