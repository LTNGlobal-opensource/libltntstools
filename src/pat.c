#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
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
	dprintf(fd, "pat.transport_stream_id = 0x%x\n", pat->transport_stream_id);
	dprintf(fd, "pat.version_number = 0x%02x\n", pat->version_number);
	dprintf(fd, "pat.current_next_indicator = %d\n", pat->current_next_indicator);
	dprintf(fd, "pat.program_count = %d\n", pat->program_count);
	for (int i = 0; i < pat->program_count; i++) {
		dprintf(fd, "\tpat.entry[%d].program_number = %d\n", i,
			pat->programs[i].program_number);
		dprintf(fd, "\tpat.entry[%d].program_map_PID = 0x%04x\n", i,
			pat->programs[i].program_map_PID);

		struct ltntstools_pmt_s *pmt = &pat->programs[i].pmt;
		dprintf(fd, "\tpat.entry[%d].pmt\n", i);
		dprintf(fd, "\t\tpmt.version_number = 0x%02x\n", pmt->version_number);
		dprintf(fd, "\t\tpmt.program_number = %d\n", pmt->program_number);
		dprintf(fd, "\t\tpmt.current_next_indicator = %d\n", pmt->current_next_indicator);
		dprintf(fd, "\t\tpmt.PCR_PID = 0x%04x\n", pmt->PCR_PID);
		dprintf(fd, "\t\tpmt.descriptor_count = %d\n", pmt->descr_list.count);
		for (int j = 0; j < pmt->descr_list.count; j++) {
			dprintf(fd, "\t\t\tpmt.descr[%d].tag = 0x%02x  len %02x : ",
				j,
				pmt->descr_list.array[j].tag,
				pmt->descr_list.array[j].len);

			for (int z = 0; z < pmt->descr_list.array[j].len; z++)
				dprintf(fd, "%02x ", pmt->descr_list.array[j].data[z]);

			dprintf(fd, "  [");
			for (int z = 0; z < pmt->descr_list.array[j].len; z++)
				dprintf(fd, "%c",
					isprint(pmt->descr_list.array[j].data[z]) ?
						pmt->descr_list.array[j].data[z] : '.');
			dprintf(fd, "]\n");
		}

		dprintf(fd, "\t\tpmt.stream_count = %d\n", pmt->stream_count);
		for (int j = 0; j < pmt->stream_count; j++) {
			dprintf(fd, "\t\t\tpmt.entry[%d].elementary_PID = 0x%04x\n", j, pmt->streams[j].elementary_PID);
			dprintf(fd, "\t\t\tpmt.entry[%d].stream_type = 0x%02x\n", j, pmt->streams[j].stream_type);
			dprintf(fd, "\t\t\tpmt.entry[%d].descriptor_count = %d\n", j, pmt->streams[j].descr_list.count);
			for (int k = 0; k < pmt->streams[j].descr_list.count; k++) {
				dprintf(fd, "\t\t\t\tpmt.entry[%d].descr[%d].tag = 0x%02x  len %02x : ",
					j,
					k,
					pmt->streams[j].descr_list.array[k].tag,
					pmt->streams[j].descr_list.array[k].len);

				for (int z = 0; z < pmt->streams[j].descr_list.array[k].len; z++)
					dprintf(fd, "%02x ", pmt->streams[j].descr_list.array[k].data[z]);
				dprintf(fd, "  [");
				for (int z = 0; z < pmt->streams[j].descr_list.array[k].len; z++)
					dprintf(fd, "%c",
						isprint(pmt->streams[j].descr_list.array[k].data[z]) ? 
							pmt->streams[j].descr_list.array[k].data[z] : '.');
				dprintf(fd, "]\n");
			}
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
	while (e && p->program_count < LTNTSTOOLS_PAT_ENTRIES_MAX) {
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
	e->current_next_indicator = pmt->b_current_next;
	e->PCR_PID = pmt->i_pcr_pid;

	/* Outer descriptors */
	dvbpsi_descriptor_t *p_desc = pmt->p_first_descriptor;
	while (p_desc) {
		int ret = ltntstools_descriptor_list_add(&e->descr_list, p_desc->i_tag, &p_desc->p_data[0], p_desc->i_length);
		if (ret < 0) {
			/* Error, skipping. */
		}
		p_desc = p_desc->p_next;
	}

	/* Add all of the ES streams. */
	dvbpsi_pmt_es_t *p_es = pmt->p_first_es;
	while (p_es && e->stream_count < LTNTSTOOLS_PMT_ENTRIES_MAX) {
		struct ltntstools_pmt_entry_s *es = &e->streams[ e->stream_count ];
		es->stream_type = p_es->i_type;
		es->elementary_PID = p_es->i_pid;

		/* Inner descriptors */
		dvbpsi_descriptor_t *p_desc = p_es->p_first_descriptor;
		while (p_desc) {
			int ret = ltntstools_descriptor_list_add(&es->descr_list, p_desc->i_tag, &p_desc->p_data[0], p_desc->i_length);
			if (ret < 0) {
				/* Error, skipping. */
			}
			p_desc = p_desc->p_next;
		}

		e->stream_count++;
		p_es = p_es->p_next;
	}
}

int ltntstools_pmt_entry_compare(struct ltntstools_pmt_entry_s *a, struct ltntstools_pmt_entry_s *b)
{
	if (a->stream_type != b->stream_type)
		return -1;
	if (a->elementary_PID != b->elementary_PID)
		return -1;

	return 0; /* Identical */
}

int ltntstools_pmt_compare(struct ltntstools_pmt_s *a, struct ltntstools_pmt_s *b)
{
	if (a->version_number != b->version_number)
		return -1;
	if (a->program_number != b->program_number)
		return -1;
	if (a->PCR_PID != b->PCR_PID)
		return -1;
	if (a->current_next_indicator != b->current_next_indicator)
		return -1;
	if (a->stream_count != b->stream_count)
		return -1;

	for (int i = 0; i < a->stream_count; i++) {
		if (ltntstools_pmt_entry_compare(&a->streams[i], &b->streams[i]) != 0)
			return -1;
	}

	return 0; /* Identical */
}

int ltntstools_pat_program_compare(struct ltntstools_pat_program_s *a, struct ltntstools_pat_program_s *b)
{
	if (a->program_number != b->program_number)
		return -1;
	if (a->program_map_PID != b->program_map_PID)
		return -1;

	return ltntstools_pmt_compare(&a->pmt, &b->pmt);
}

int ltntstools_pat_compare(struct ltntstools_pat_s *a, struct ltntstools_pat_s *b)
{
	if (a->transport_stream_id != b->transport_stream_id)
		return -1;
	if (a->version_number != b->version_number)
		return -1;
	if (a->current_next_indicator != b->current_next_indicator)
		return -1;
	if (a->program_count != b->program_count)
		return -1;

	for (int i = 0; i < a->program_count; i++) {
		if (ltntstools_pat_program_compare(&a->programs[i], &b->programs[i]) != 0)
			return -1;
	}

	return 0; /* Identical */
}

int ltntstools_pat_enum_services_scte35(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr, uint16_t *pid)
{
	if (!pat || !pmtptr || !e || !pid)
		return -1;

	if ((*e) + 1 > pat->program_count)
		return -1;

	*pmtptr = NULL;
	*pid = 0;

	for (int i = 0; i < pat->program_count; i++) {

		struct ltntstools_pmt_s *pmt = &pat->programs[*e].pmt;

		if (ltntstools_descriptor_list_contains_scte35_cue_registration(&pmt->descr_list) == 0) {
			continue;
		}

		/* Find the SCTE35 pid */
		for (int j = 0; j < pmt->stream_count; j++) {
			if (pmt->streams[j].stream_type == 0x86) {
				*pid = pmt->streams[j].elementary_PID;
				*pmtptr = pmt;
				(*e)++;
				return 0; /* Success */
			}
		}

	}

	return -1; /* Error */
}

int ltntstools_pat_enum_services_smpte2038(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr, uint16_t *pid)
{
	if (!pat || !pmtptr || !e || !pid)
		return -1;

	if ((*e) + 1 > pat->program_count)
		return -1;

	*pmtptr = NULL;
	*pid = 0;

	for (int i = 0; i < pat->program_count; i++) {

		struct ltntstools_pmt_s *pmt = &pat->programs[*e].pmt;

		for (int j = 0; j < pmt->stream_count; j++) {
			struct ltntstools_pmt_entry_s *se = &pmt->streams[j];

			if (se->stream_type != 0x06) {
				continue;
			}

			if (ltntstools_descriptor_list_contains_smpte2038_registration(&se->descr_list) == 0) {
				continue;
			}

			*pid = se->elementary_PID;
			*pmtptr = pmt;
			(*e)++;
			return 0; /* Success */
		}
	}

	return -1; /* Error */
}

int ltntstools_pmt_query_video_pid(struct ltntstools_pmt_s *pmt, uint16_t *pid, uint8_t *estype)
{
	if (!pmt || !pid || !estype)
		return -1;

	for (int j = 0; j < pmt->stream_count; j++) {
		if (ltntstools_is_ESPayloadType_Video(pmt->streams[j].stream_type)) {
			*pid = pmt->streams[j].elementary_PID;
			*estype = pmt->streams[j].stream_type;

			return 0; /* Success */
		}
	}

	return -1; /* Failed */
}

int ltntstools_pat_enum_services_video(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr)
{
	if (!pmtptr || !e)
		return -1;

	if ((*e) + 1 > pat->program_count)
		return -1;

	for (int i = (*e); i < pat->program_count; i++) {

		struct ltntstools_pmt_s *pmt = &pat->programs[*e].pmt;

		for (int j = 0; j < pmt->stream_count; j++) {
			
			if (ltntstools_is_ESPayloadType_Video(pmt->streams[j].stream_type)) {
				(*e)++;
				*pmtptr = pmt;
				return 0; /* Success */
			}
			
		}
		(*e)++;
	}

	return -1; /* Failed */
}
