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

struct ltntstools_pat_s *ltntstools_pat_clone(struct ltntstools_pat_s *pat)
{
	if (!pat) {
		return NULL;
	}

	struct ltntstools_pat_s *dst = calloc(1, sizeof(*pat));
	if (dst) {
		memcpy(dst, pat, sizeof(*pat));
	}

	return dst;
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
		
		if (pat->programs[i].service_id == 0) {
			dprintf(fd, "\tsdt.service_id       = n/a\n");
			dprintf(fd, "\tsdt.service_type     = n/a\n");
			dprintf(fd, "\tsdt.service_name     = n/a\n");
			dprintf(fd, "\tsdt.service_provider = n/a\n");
		} else {
			dprintf(fd, "\tsdt.service_id       = 0x%04x\n", pat->programs[i].service_id);
			dprintf(fd, "\tsdt.service_type     = 0x%02x\n", pat->programs[i].service_type);
			dprintf(fd, "\tsdt.service_name     = %s\n", pat->programs[i].service_name);
			dprintf(fd, "\tsdt.service_provider = %s\n", pat->programs[i].service_provider);
		}

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

int ltntstools_pat_get_services_teletext(struct ltntstools_pat_s *pat, uint16_t **pid_array, int *pid_count)
{
    if (!pat || !pid_array || !pid_count)
        return -1;

    *pid_array = NULL;
    *pid_count = 0;

	int cnt = 0;
	for (int i = 0; i < pat->program_count; i++) {
        struct ltntstools_pmt_s *pmt = &pat->programs[i].pmt;

		for (int j = 0; j < pmt->stream_count; j++) {
			struct ltntstools_pmt_entry_s *se = &pmt->streams[j];

			if (ltntstools_descriptor_list_contains_teletext(&se->descr_list) == 0) {
				continue;
			}

			cnt++;
		}
	}

	/* Allocate memory for PIDs array */
	*pid_array = (uint16_t *)malloc(cnt * sizeof(uint16_t));
	if (!*pid_array)
		return -1; /* Memory allocation failure */

	int idx = 0;
	for (int i = 0; i < pat->program_count; i++) {
		struct ltntstools_pmt_s *pmt = &pat->programs[i].pmt;

		for (int j = 0; j < pmt->stream_count; j++) {
			struct ltntstools_pmt_entry_s *se = &pmt->streams[j];

			if (ltntstools_descriptor_list_contains_teletext(&se->descr_list) == 0) {
				continue;
			}

			(*pid_array)[idx++] = pmt->streams[j].elementary_PID;
		}
	}
	
	*pid_count = cnt;

    return 0; /* Error */
}

int ltntstools_pat_enum_services_scte35(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr, uint16_t **pid_array, int *pid_count)
{
    if (!pat || !pmtptr || !e || !pid_array || !pid_count)
        return -1;

    if ((*e) + 1 > pat->program_count)
        return -1;

    *pmtptr = NULL;
    *pid_array = NULL;
    *pid_count = 0;

    for (int i = 0; i < pat->program_count; i++) {
        struct ltntstools_pmt_s *pmt = &pat->programs[*e].pmt;

        if (ltntstools_descriptor_list_contains_scte35_cue_registration(&pmt->descr_list) == 0) {
			(*e)++;
            continue;
        }

        /* Allocate memory for PIDs array */
        *pid_array = (uint16_t *)malloc(pmt->stream_count * sizeof(uint16_t));
        if (!*pid_array)
            return -1; /* Memory allocation failure */

        /* Find all SCTE-35 PIDs */
        for (int j = 0; j < pmt->stream_count; j++) {
            if (pmt->streams[j].stream_type == 0x86) {
                (*pid_array)[*pid_count] = pmt->streams[j].elementary_PID;
                (*pid_count)++;
            }
        }

        if (*pid_count > 0) {
            *pmtptr = pmt;
            (*e)++;
            return 0; /* Success */
        } else {
            /* Free the allocated memory if no PIDs were found */
            free(*pid_array);
            *pid_array = NULL;
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

	for (int i = *e; i < pat->program_count; i++) {

		struct ltntstools_pmt_s *pmt = &pat->programs[i].pmt;

		if (pmt->program_number == 0) {
			(*e)++;
			continue;
		}

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
		(*e)++;
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

int ltntstools_pmt_entry_is_audio(const struct ltntstools_pmt_entry_s *pmt)
{
	if (ltntstools_is_ESPayloadType_Audio(pmt->stream_type)) {
		return 1;
	}
	if (pmt->stream_type != 0x06) { /* 13818-1 PES private data */
		return 0;
	}

	for (uint32_t i = 0; i < pmt->descr_list.count; ++i) {
		switch (pmt->descr_list.array[i].tag) {
			/* MPEG descriptors */
			case 0x03: /* Audio stream descriptor */
			case 0x1c: /* MPEG-4 audio descriptor */
			case 0x2b: /* MPEG-2 AAC audio descriptor */

			/* DVB descriptors */
			case 0x6a: /* AC-3 descriptor */
			case 0x7a: /* Enhanced AC-3 descriptor */
			case 0x7c: /* AAC descriptor */

			return pmt->descr_list.array[i].tag;;
		}
	}

	return 0;
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

int ltntstools_pat_enum_services_teletext(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr)
{
	if (!pmtptr || !e)
		return -1;

	if ((*e) + 1 > pat->program_count)
		return -1;

	for (int i = (*e); i < pat->program_count; i++) {

		struct ltntstools_pmt_s *pmt = &pat->programs[*e].pmt;

		for (int j = 0; j < pmt->stream_count; j++) {
			struct ltntstools_pmt_entry_s *se = &pmt->streams[j];

			if (ltntstools_descriptor_list_contains_teletext(&se->descr_list)) {
				(*e)++;
				*pmtptr = pmt;
				return 0; /* Success */
			}
			
		}
		(*e)++;
	}

	return -1; /* Failed */
}

int ltntstools_pat_enum_services_audio(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr, uint32_t **stream_type_array, uint16_t **pid_array, int *pid_count)
{
	if (!pat || !pmtptr || !e || !pid_array || !pid_count || !stream_type_array)
		return -1;

	if ((*e) + 1 > pat->program_count)
		return -1;

	*pmtptr = NULL;
	*pid_array = NULL;
	*stream_type_array = NULL;
	*pid_count = 0;

	for (int i = 0; i < pat->program_count; i++) {
		struct ltntstools_pmt_s *pmt = &pat->programs[*e].pmt;

		/* Allocate memory for PIDs array */
		*pid_array = (uint16_t *)malloc(pmt->stream_count * sizeof(uint16_t));
		if (!*pid_array)
			return -1; /* Memory allocation failure */

		/* Allocate memory for stream type array */
		*stream_type_array = (uint32_t *)malloc(pmt->stream_count * sizeof(uint32_t));
		if (!*stream_type_array) {
			free(*pid_array);
			*pid_array = NULL;
			return -1; /* Memory allocation failure */
		}

		/* Find all audio PIDs */
		int found = 0;
		for (int j = 0; j < pmt->stream_count; j++) {
			struct ltntstools_pmt_entry_s *stream = &pmt->streams[j];
			if (ltntstools_pmt_entry_is_audio(stream)) {
				(*pid_array)[*pid_count] = stream->elementary_PID;
				(*stream_type_array)[*pid_count] = stream->stream_type;
				(*pid_count)++;
				found += 1;
			}
		}

		if (found > 0 && *pid_count > 0) {
			*pmtptr = pmt;
			(*e)++;
			return 0; /* Success */
		} else {
			/* Free the allocated memory if no PIDs were found */
			free(*pid_array);
			*pid_array = NULL;
			free(*stream_type_array);
			*stream_type_array = NULL;
			(*e)++;
		}
	}

	return -1; /* Error */
}

int ltntstools_pat_enum_services(struct ltntstools_pat_s *pat, int *e, uint16_t pid, struct ltntstools_pmt_s **pmtptr)
{
	if (!pmtptr || !e)
		return -1;

	if ((*e) + 1 > pat->program_count)
		return -1;

	for (int i = (*e); i < pat->program_count; i++) {

		struct ltntstools_pmt_s *pmt = &pat->programs[*e].pmt;

		if (pat->programs[*e].program_map_PID == pid) {
			(*e)++;
			*pmtptr = pmt;
			return 0; /* Success */
		}
		(*e)++;
	}

	return -1; /* Failed */
}

int ltntstools_pmt_remove_es_for_pid(struct ltntstools_pmt_s *pmt, uint16_t pid)
{
	if (!pmt)
		return -1;

	for (int i = 0; i < pmt->stream_count; i++) {
		struct ltntstools_pmt_entry_s *e = &pmt->streams[i];
		if (e->elementary_PID == pid) {
			if (i == pmt->stream_count - 1) {
				/* Last element in the last */
				pmt->stream_count = pmt->stream_count - 1;
				break;
			} else {
				/* Remove one entry from the middle or start of the list */
				int count = pmt->stream_count - i - 1;
				memmove(&pmt->streams[i], &pmt->streams[i + 1], count * sizeof(struct ltntstools_pmt_entry_s));
				pmt->stream_count = pmt->stream_count - 1;
				break;
			}
		}
	}
	return 0; /* Success */
}

/* DVBPSI - I don't like this fun, I don't think we need it. TODO, remove in future. */
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

/* Straight out of the libdvbpsi sample project. */
static void writePSI(uint8_t *p_packet, dvbpsi_psi_section_t *p_section)
{
  p_packet[0] = 0x47;

  while(p_section)
  {
    uint8_t* p_pos_in_ts;
    uint8_t* p_byte = p_section->p_data;
    uint8_t* p_end  = p_section->p_payload_end + (p_section->b_syntax_indicator ? 4 : 0);

    p_packet[1] |= 0x40;
    p_packet[3]  = (p_packet[3] & 0x0f) | 0x10;
    p_packet[4]  = 0x00; /* pointer_field */
    p_pos_in_ts  = p_packet + 5;

    while((p_pos_in_ts < p_packet + 188) && (p_byte < p_end)) {
      *(p_pos_in_ts++) = *(p_byte++);
	}

    while(p_pos_in_ts < p_packet + 188) {
      *(p_pos_in_ts++) = 0xff;
	}

    p_packet[3] = (p_packet[3] + 1) & 0x0f;

    while(p_byte < p_end) {
      p_packet[1] &= 0xbf;
      p_packet[3]  = (p_packet[3] & 0x0f) | 0x10;
      p_pos_in_ts  = p_packet + 4;

      while((p_pos_in_ts < p_packet + 188) && (p_byte < p_end)) {
        *(p_pos_in_ts++) = *(p_byte++);
	  }

      while(p_pos_in_ts < p_packet + 188) {
        *(p_pos_in_ts++) = 0xff;
	  }

      p_packet[3] = (p_packet[3] + 1) & 0x0f;
    }

    p_section = p_section->p_next;
  }
}

int ltntstools_pat_create_packet_ts(struct ltntstools_pat_s *pat, uint8_t cc, uint8_t *packet, int packetLength)
{
	if ((!pat) || (packetLength != 188) || (packet == NULL))
		return -1;

	uint8_t *p = packet;
	int i = 0;

	memset(p, 0xFF, packetLength);

	p[i++] = 0x47;
	p[i++] = 0x40;
	p[i++] = 0x00;
	p[i++] = 0x10 | (cc & 0x0f);
	p[i++] = 0x00;

	p[i++] = 0x00; /* PAT Table */
	p[i++] = 0xB0;

	p[i++] = 9 + (pat->program_count * 4);

	p[i++] = pat->transport_stream_id >> 8;
	p[i++] = pat->transport_stream_id;

	p[i++] = 0xC0 | pat->current_next_indicator;
	p[i++] = 0x00; /* Section */
	p[i++] = 0x00; /* last section */

	for (int j = 0; j < pat->program_count; j++) {
		p[i++] = pat->programs[j].pmt.program_number >> 8;
		p[i++] = pat->programs[j].pmt.program_number;
		p[i++] = 0xE0 | ((pat->programs[j].program_map_PID >> 8) & 0x1F);
		p[i++] = pat->programs[j].program_map_PID & 0xFF;
	}

	uint32_t crc;
	ltntstools_getCRC32(&p[5], i - 5, &crc);

	p[i++] = (crc >> 24) & 0xFF;
	p[i++] = (crc >> 16) & 0xFF;
	p[i++] = (crc >> 8) & 0xFF;
	p[i++] = crc & 0xFF;

	return 0; /* Success */
}

int ltntstools_pmt_create_packet_ts(struct ltntstools_pmt_s *pmt, uint16_t pid, uint8_t cc, uint8_t *packet, int packetLength)
{
	if ((!pmt) || (packetLength != 188) || (packet == NULL))
		return -1;

	uint8_t *p = packet;
	int i = 0;

	memset(p, 0xFF, packetLength);

	p[i++] = 0x47;
	p[i++] = 0x40 | ((pid & 0x1fff) >> 8);
	p[i++] = pid & 0xff;
	p[i++] = 0x10 | (cc & 0x0f);
	p[i++] = 0x00;

	p[i++] = 0x02; /* PMT Table */
	p[i++] = 0xB0;

	p[i++] = 9 + 4 + (pmt->stream_count * 5); /* Length */

	p[i++] = pmt->program_number >> 8;
	p[i++] = pmt->program_number;
	p[i++] = 0xc3;
	p[i++] = 0x00; /* section */
	p[i++] = 0x00; /* last section */

	p[i++] = pmt->PCR_PID >> 8;
	p[i++] = pmt->PCR_PID;

	p[i++] = 0xf0; 
	p[i++] = 0x00;

	for (int j = 0; j < pmt->stream_count; j++) {
		p[i++] = pmt->streams[j].stream_type;
		p[i++] = pmt->streams[j].elementary_PID >> 8;
		p[i++] = pmt->streams[j].elementary_PID;
		p[i++] = 0xf0; 
		p[i++] = 0x00;
	}

	uint32_t crc;
	ltntstools_getCRC32(&p[5], i - 5, &crc);

	p[i++] = (crc >> 24) & 0xFF;
	p[i++] = (crc >> 16) & 0xFF;
	p[i++] = (crc >> 8) & 0xFF;
	p[i++] = crc & 0xFF;

	return 0; /* Success */
}

int ltntstools_pmt_create_packet_ts2(struct ltntstools_pmt_s *p, uint16_t pid, uint8_t cc, uint8_t *packet, int packetLength)
{
	if ((!p) || (packetLength != 188) || (packet == NULL))
		return -1;

	dvbpsi_pmt_t *pmt = dvbpsi_pmt_new(p->program_number, p->version_number, p->current_next_indicator, p->PCR_PID);

	for (int j = 0; j < p->descr_list.count; j++) {
		dvbpsi_pmt_descriptor_add(pmt,
			p->descr_list.array[j].tag,
			p->descr_list.array[j].len,
			&p->descr_list.array[j].data[0]);
	}

	for (int i = 0; i < p->stream_count; i++) {
		dvbpsi_pmt_es_t *es = dvbpsi_pmt_es_add(pmt, p->streams[i].stream_type, p->streams[i].elementary_PID);

		for (int j = 0; j < p->streams[i].descr_list.count; j++) {
			dvbpsi_pmt_es_descriptor_add(es,
				p->streams[i].descr_list.array[j].tag,
				p->streams[i].descr_list.array[j].len, &p->streams[i].descr_list.array[j].data[0]);
		}
	}

	dvbpsi_t *dvbpsi = dvbpsi_new(&message, DVBPSI_MSG_ERROR);

	dvbpsi_psi_section_t *sec = dvbpsi_pmt_sections_generate(dvbpsi, pmt);

	memset(packet, 0, 188);
	packet[0] = 0x47;
	packet[1] = (pid & 0x1fff) >> 8;
	packet[2] = pid & 0xff;
	packet[3] = 0x00 | (cc & 0x0f);
	writePSI(packet, sec);

	dvbpsi_DeletePSISections(sec);
	dvbpsi_pmt_delete(pmt);
	dvbpsi_delete(dvbpsi);

	//ltntstools_hexdump(packet, 188, 32);

	return 0; /* Success */
}

const struct ltntstools_pmt_entry_s **ltntstools_pmt_enum_services_audio(const struct ltntstools_pmt_s *pmt, int *pid_count)
{
	*pid_count = 0;

	/* Allocate memory for PIDs array */
	const struct ltntstools_pmt_entry_s **streams = malloc(pmt->stream_count * sizeof(struct ltntstools_pmt_entry_s*));
	if (!streams)
		return NULL; /* Memory allocation failure */

	/* Find all audio PIDs */
	for (int j = 0; j < pmt->stream_count; j++) {
		const struct ltntstools_pmt_entry_s *stream = &pmt->streams[j];
		if (ltntstools_pmt_entry_is_audio(stream)) {
			streams[(*pid_count)++] = stream;
		}
	}	

	if (*pid_count <= 0) {
		/* Free the allocated memory if no PIDs were found */
		free(streams);
		streams = NULL;
	}

	return streams;
}
