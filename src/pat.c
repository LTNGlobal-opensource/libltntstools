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
	dprintf(fd, "\tcurrent_next = 0x%x\n", pat->current_next);
	dprintf(fd, "\tprogram_count = %d\n", pat->program_count);
	for (int i = 0; i < pat->program_count; i++) {
		dprintf(fd, "\t\tprogram_number = %d, pid 0x%04x\n",
			pat->programs[i].program_number,
			pat->programs[i].pid);
	}
}
