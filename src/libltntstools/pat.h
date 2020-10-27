#ifndef TR101290_PAT_H
#define TR101290_PAT_H

/* A static single memory allocation that prepresents a PAT
 * and minimal PMT stream description, primarily used by the streammodel
 * as a result type to callers, when they ask for the state of the current
 * model.
 *
 * The single allocation should always be copied with a single memcpy, lets
 * avoid pointers, intensionally.
 *
 * Caveats:
 *  PATs can ONLY carry a maximum of 64 programs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* See ISO13818-1 "program_association_section()" */
struct ltntstools_pat_program_s
{
	uint32_t program_number;
	uint32_t pid;
};

struct ltntstools_pat_s
{
	uint32_t transport_stream_id;
	uint32_t version;
	uint32_t current_next_indicator;

	uint32_t program_count;
	struct   ltntstools_pat_program_s programs[64];
};

struct ltntstools_pat_s *ltntstools_pat_alloc();
void ltntstools_pat_free(struct ltntstools_pat_s *pat);
void ltntstools_pat_dprintf(struct ltntstools_pat_s *pat, int fd);

typedef struct dvbpsi_pat_s dvbpsi_pat_t;
struct ltntstools_pat_s * ltntstools_pat_alloc_from_existing(dvbpsi_pat_t *pat);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_PAT_H */
