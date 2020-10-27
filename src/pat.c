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
#include <dvbpsi/descriptor.h>
#include <dvbpsi/pmt.h>
#include <dvbpsi/dr.h>

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
	dprintf(fd, "\tversion_number = 0x%x\n", pat->version_number);
	dprintf(fd, "\tcurrent_next_indicator = 0x%x\n", pat->current_next_indicator);
	dprintf(fd, "\tprogram_count = %d\n", pat->program_count);
	for (int i = 0; i < pat->program_count; i++) {
		dprintf(fd, "\t\tPROGRAM\n");
		dprintf(fd, "\t\t\tprogram_number = %d, pid 0x%04x\n",
			pat->programs[i].program_number,
			pat->programs[i].program_map_PID);

		struct ltntstools_pmt_s *pmt = &pat->programs[i].pmt;
		dprintf(fd, "\t\t\tPMT\n");
		dprintf(fd, "\t\t\t\tversion_number = %d\n", pmt->version_number);
		dprintf(fd, "\t\t\t\tprogram_number = %d\n", pmt->program_number);
		dprintf(fd, "\t\t\t\tPCR_PID = 0x%04x\n", pmt->PCR_PID);
		dprintf(fd, "\t\t\t\tstream_count = %d\n", pmt->stream_count);

		for (int j = 0; j < pmt->stream_count; j++) {
			dprintf(fd, "\t\t\t\t\telementary_PID = 0x%04x\n", pmt->streams[j].elementary_PID);
			dprintf(fd, "\t\t\t\t\tstream_type = 0x%02x\n", pmt->streams[j].stream_type);
		}
	}
}

struct ltntstools_pat_s * ltntstools_pat_alloc_from_existing(dvbpsi_pat_t *pat)
{
	if (!pat)
		return NULL;

	struct ltntstools_pat_s *p = ltntstools_pat_alloc();

	/* Convert the dvbpsi struct into a new obj. */
	p->transport_stream_id = pat->i_ts_id;
	p->version_number = pat->i_version;
	p->current_next_indicator = pat->b_current_next;

	dvbpsi_pat_program_t *e = pat->p_first_program;
	while (e) {
		p->programs[p->program_count].program_number = e->i_number;
		p->programs[p->program_count].program_map_PID = e->i_pid;
		p->program_count++;
		e = e->p_next;
	}

	return p;
}

void ltntstools_pat_add_from_existing(struct ltntstools_pat_s *pat, dvbpsi_pmt_t *pmt)
{
	if (!pat || !pmt)
		return;

	/* Find the program in the PAT for this PMT. */
	struct ltntstools_pat_program_s *pp = NULL;
	struct ltntstools_pmt_s *e = NULL;
	for (int i = 0; i < pat->program_count; i++) {
		if (pat->programs[i].program_number == pmt->i_program_number) {
			pp = &pat->programs[i];
			e = &pat->programs[i].pmt;
			break;
		}
	}
	if (pp == NULL) {
		fprintf(stderr, "%s() sbhould never happen\n", __func__);
		return;
	}

	/* For this PMT, append configure the PCR pid and add any ES streams */
	e->version_number = pmt->i_version;
	e->program_number = pmt->i_program_number;
	e->PCR_PID = pmt->i_pcr_pid;

	/* Add all of the ES streams. */
	dvbpsi_pmt_es_t *p_es = pmt->p_first_es;
	while (p_es) {
		struct ltntstools_pmt_entry_s *es = &e->streams[ e->stream_count ];
		es->stream_type = p_es->i_type;
		es->elementary_PID = p_es->i_pid;
		e->stream_count++;
		p_es = p_es->p_next;
	}
}
