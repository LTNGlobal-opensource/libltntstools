#ifndef STREAMMODEL_TYPES_H
#define STREAMMODEL_TYPES_H

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

#if 0
#define DVBPSI_REPORTING (DVBPSI_MSG_DEBUG)
#else
#define DVBPSI_REPORTING (DVBPSI_MSG_ERROR)
#endif

/* Any given PID could have multiple PMTs on a single pid.
 * We need to track all of this state.
 */
struct streammodel_pid_parser_s
{
	dvbpsi_t *p_dvbpsi;
	dvbpsi_pmt_t *p_pmt;
	int programNumber;
};

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
#define MAX_PID_PARSERS 24
	struct streammodel_pid_parser_s parser[MAX_PID_PARSERS];
	dvbpsi_pat_t *p_pat;

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
	struct timeval pmtCollectionTimer; /* We have until this time expires to collect all PMTs, else
					    * We have a mis-configured stream, or the stream changed behind the frameworks back
					    * during PMT collection, and the new PMT never arrived.
					    */

#define MAX_ROM_PIDS 0x2000
	struct streammodel_pid_s pids[MAX_ROM_PIDS];

	/* Housekeeping */
	struct streammodel_ctx_s *ctx;
};

struct se_array_item_s
{
	uint16_t pid;
	uint8_t  tableId;
	char    *name;
	void    *hdl;
	int      complete;
	int      crcValid;
	uint64_t packetCounts;
	uint32_t context;  /* STREAMMODEL_CB_CONTEXT_PAT */

};

struct streammodel_ctx_s
{
	/* The framework builds two working models of the incoming stream, A and B.
	 * The stream is assumed to change every 500ms, so we constantly build a current model.
	 * 'current' points to the last known good model. 
	 * into 'current' when its considered complete and its safe to switch pointers.
	 */
	uint64_t currentModelVersion;
	pthread_mutex_t rom_mutex;
	struct streammodel_rom_s *current;	/* Model any user queries will run against. */
	struct streammodel_rom_s *next;		/* Model currently being build by the framework, never user accessible. */
	struct streammodel_rom_s roms[2];	/* Storage for the models. */
	void *userContext;                  /* User specific pointer, return to the caller during callbacks. */

	/* Housekeeping */
	struct timeval now;			/* Each write() call updates this */

	/* */
	int writePackets;
	int restartModel;

	/* TR101290 CRC32 checks - see streammodel-extractors.c */
	int   enableSectionCRCChecks;
	int   seCount;
	struct se_array_item_s *seArray;
	ltntstools_streammodel_callback cb;
};

int  extractors_alloc(struct streammodel_ctx_s *ctx);
int  extractors_add(struct streammodel_ctx_s *ctx, uint16_t pid, uint8_t tableId, char *name, uint32_t context);
int  extractors_write(struct streammodel_ctx_s *ctx, const uint8_t *pkts, int packetCount);
void extractors_free(struct streammodel_ctx_s *ctx);

#endif /* STREAMMODEL_TYPES_H */