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
	dprintf(fd, "\tcurrent_next_indicator = 0x%x\n", pat->current_next_indicator);
	dprintf(fd, "\tprogram_count = %d\n", pat->program_count);
	for (int i = 0; i < pat->program_count; i++) {
		dprintf(fd, "\t\tprogram_number = %d, pid 0x%04x\n",
			pat->programs[i].program_number,
			pat->programs[i].pid);
	}
}

struct ltntstools_pat_s * ltntstools_pat_alloc_from_existing(dvbpsi_pat_t *pat)
{
	if (!pat)
		return NULL;

	struct ltntstools_pat_s *p = ltntstools_pat_alloc();

	/* Convert the dvbpsi struct into a new obj. */
	p->transport_stream_id = pat->i_ts_id;
	p->version = pat->i_version;
	p->current_next_indicator = pat->b_current_next;

	dvbpsi_pat_program_t *e = pat->p_first_program;
	while (e) {
		p->programs[p->program_count].program_number = e->i_number;
		p->programs[p->program_count].pid = e->i_pid;
		p->program_count++;
		e = e->p_next;
	}

	return p;
}

