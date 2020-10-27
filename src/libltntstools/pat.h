#ifndef TR101290_PAT_H
#define TR101290_PAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ltntstools_pat_program_s
{
	uint32_t program_number;
	uint32_t pid;
};

struct ltntstools_pat_s
{
	uint32_t transport_stream_id;
	uint32_t version;
	uint32_t current_next;

	uint32_t program_count;
	struct   ltntstools_pat_program_s programs[64];
};
struct ltntstools_pat_s *ltntstools_pat_alloc();
void ltntstools_pat_free(struct ltntstools_pat_s *pat);
void ltntstools_pat_dprintf(struct ltntstools_pat_s *pat, int fd);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_PAT_H */
