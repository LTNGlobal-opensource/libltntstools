#ifndef DEMUX_TYPES_H
#define DEMUX_TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>

#include "libltntstools/streammodel.h"
#include "libltntstools/demux.h"
#include "libltntstools/ts.h"
#include "libltntstools/pat.h"
#include "libltntstools/timeval.h"

#include "xorg-list.h"

#define MODULE_PREFIX "demux: "
#define MAX_PIDS 8192

#define _getPID(ctx, pidNr) (&ctx->pids[ pidNr & 0x1fff])

enum payload_e
{
	P_UNDEFINED = 0,
	P_AUDIO,
	P_VIDEO,
	P_SMPTE2064,
};

struct demux_pid_pes_item_s
{
	struct xorg_list list;
	struct demux_pid_s *pid;
	struct ltn_pes_packet_s *pes;
	struct timeval ts;
};

struct demux_pid_s
{
	uint16_t pidNr;
	enum payload_e payload;

	struct xorg_list pesList; /**< list of call PES frames FIFO arrangement */
	uint32_t pesListCount;
	pthread_cond_t pesListItemAdd;
	pthread_mutex_t pesListMutex;

	void *pe; /**< PESExtractor handle */
};

struct demux_ctx_s
{
	int verbose;
	void *userContext;

	const struct ltntstools_pat_s *pat;

	struct demux_pid_s pids[MAX_PIDS];
};

void *demux_pid_pe_callback(void *userContext, struct ltn_pes_packet_s *pes);
void demux_pid_init(struct demux_pid_s *pid, uint16_t pidNr);
void demux_pid_uninit(struct demux_pid_s *pid);
void demux_pid_set_estype(struct demux_pid_s *pid, enum payload_e estype);

uint32_t _itemAgeMs(struct demux_pid_pes_item_s *item, struct timeval *now);

#endif /* DEMUX_TYPES_H */

