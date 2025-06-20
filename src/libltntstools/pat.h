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
 *  Each program can have a maximum of 16 ES streams.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libltntstools/descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       PMT table entry. an ES stream type and related descriptors.
 */
struct ltntstools_pmt_entry_s
{
	uint32_t stream_type;
	uint32_t elementary_PID;

	struct ltntstools_descriptor_list_s descr_list;
};
int ltntstools_pmt_entry_compare(struct ltntstools_pmt_entry_s *a, struct ltntstools_pmt_entry_s *b);

/**
 * @brief       PMT table. A collection of ES streams and descriptors
 */
struct ltntstools_pmt_s
{
	uint32_t version_number;
	uint32_t program_number;
	uint32_t current_next_indicator;
	uint32_t PCR_PID;

	uint32_t stream_count;
#define LTNTSTOOLS_PMT_ENTRIES_MAX 16
	struct ltntstools_pmt_entry_s streams[LTNTSTOOLS_PMT_ENTRIES_MAX];

	struct ltntstools_descriptor_list_s descr_list;
};
int ltntstools_pmt_compare(struct ltntstools_pmt_s *a, struct ltntstools_pmt_s *b);

/**
 * @brief       For a given PMT object, search of the ES stream using PID X and remove it.
 * @param[in]   struct ltntstools_pmt_s *pmt - object
 * @param[in]   uint16_t pid - final TS pid for output packet.
 * @return      0 - Success or, < 0 on error.
 */
int ltntstools_pmt_remove_es_for_pid(struct ltntstools_pmt_s *pmt, uint16_t pid);

/**
 * @brief       Convert a pmt object into a fully formed transport packet (EXACTLY ONE packet, not longer). TODO. Support longer PMTs.
 * @param[in]   struct ltntstools_pmt_s *pmt - object
 * @param[in]   uint16_t pid - final TS pid for output packet.
 * @param[in]   uint8_t cc - desired initial continuity counter value
 * @param[in]   uint8_t *packet - Buffer where packet will be created
 * @param[in]   int packetLengthBytes - Buffer length, must be exactly 188 bytes.
 * @return      0 - Success or, < 0 on error.
 */
int ltntstools_pmt_create_packet_ts(struct ltntstools_pmt_s *pmt, uint16_t pid, uint8_t cc, uint8_t *packet, int packetLengthBytes);

/**
 * @brief       PAT table entry
 */
struct ltntstools_pat_program_s
{
	uint32_t program_number;
	uint32_t program_map_PID;

	struct ltntstools_pmt_s pmt;

	/* Service type, name and provider, if available. */
	uint16_t service_id;
	uint8_t  service_type;
	uint8_t  service_name[32];
	uint8_t  service_provider[32];
};
int ltntstools_pat_program_compare(struct ltntstools_pat_program_s *a, struct ltntstools_pat_program_s *b);

/**
 * @brief       PAT table. A collection of PMTs. See ISO13818-1 "program_association_section()"
 */
struct ltntstools_pat_s
{
	uint32_t transport_stream_id;
	uint32_t version_number;
	uint32_t current_next_indicator;
	struct   ltntstools_descriptor_list_s descr_list;

	uint32_t program_count;
#define LTNTSTOOLS_PAT_ENTRIES_MAX 64
	struct   ltntstools_pat_program_s programs[LTNTSTOOLS_PAT_ENTRIES_MAX];
};
int ltntstools_pat_compare(struct ltntstools_pat_s *a, struct ltntstools_pat_s *b);

struct ltntstools_pat_s *ltntstools_pat_alloc();
void ltntstools_pat_free(struct ltntstools_pat_s *pat);
void ltntstools_pat_dprintf(struct ltntstools_pat_s *pat, int fd);

/**
 * @brief       Duplicate the entire PAT object into a new memory allocation.
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @return      New allocation on Success, else NULL.
 */
struct ltntstools_pat_s *ltntstools_pat_clone(struct ltntstools_pat_s *pat);

/* Helper functions from libdvbpsi to the internal model. */
typedef struct dvbpsi_pat_s dvbpsi_pat_t;
struct ltntstools_pat_s * ltntstools_pat_alloc_from_existing(dvbpsi_pat_t *pat);

typedef struct dvbpsi_pmt_s dvbpsi_pmt_t;
void ltntstools_pat_add_from_existing(struct ltntstools_pat_s *pat, dvbpsi_pmt_t *pmt);

/**
 * @brief       Convert a pat object into a fully formed transport packet (EXACTLY ONE packet, not longer). TODO. Support longer PATs.
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[in]   uint8_t cc - desired initial continuity counter value
 * @param[in]   uint8_t *packet - Buffer where packet will be created
 * @param[in]   int packetLengthBytes - Buffer length, must be exactly 188 bytes.
 * @return      0 - Success or, < 0 on error.
 */
int ltntstools_pat_create_packet_ts(struct ltntstools_pat_s *pat, uint8_t cc, uint8_t *packet, int packetLengthBytes);

/**
 * @brief       Enumerate all services in the PAT object, find any SCTE35 pids and return the associated PMT (and pid).
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[in]   int *e - used internally to enumerate objects. Pass 0 value int on first call then don't modify afterwards
 * @param[out]  struct ltntstools_pmt_s **pmt - ptr to the pmt object containing in the PAT
 * @return      0 - Success, PMT and PID contain details. < 0, no nore SCTE35 sevices, or error.
 */
int ltntstools_pat_enum_services_scte35(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr, uint16_t **pid_array, int *pid_count);

/**
 * @brief       Enumerate all services in the PAT object, find any Teletext/OP47/WST pids and return and array of pids
 *              Caller is responsible for freeing the array after use.
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[out]  uint16_t **pid_array - list of pids containing OP47/Teletext services
 * @param[out]  int *pid_count - number of elements in the list
 * @return      0 - Success, PMT and PID contain details. < 0, no nore SCTE35 sevices, or error.
 */
int ltntstools_pat_get_services_teletext(struct ltntstools_pat_s *pat, uint16_t **pid_array, int *pid_count);

/**
 * @brief       Enumerate all services in the PAT object, find any OP47/Teletext pids and return the associated PMT (and pid).
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[in]   int *e - used internally to enumerate objects. Pass 0 value int on first call then don't modify afterwards
 * @param[out]  struct ltntstools_pmt_s **pmt - ptr to the pmt object containing in the PAT
 * @return      0 - Success, PMT and PID contain details. < 0, no nore SCTE35 sevices, or error.
 */
int ltntstools_pat_enum_services_teletext(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmtptr);

/**
 * @brief       Enumerate all services in the PAT object, find any SMPTE2038 pids and return the associated PMT (and pid).
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[in]   int *e - used internally to enumerate objects. Pass 0 value int on first call then don't modify afterwards
 * @param[out]  struct ltntstools_pmt_s **pmt - ptr to the pmt object containing in the PAT
 * @return      0 - Success, PMT and PID contain details. < 0, no nore SCTE35 sevices, or error.
 */
int ltntstools_pat_enum_services_smpte2038(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmt, uint16_t *pid);

/**
 * @brief       Enumerate all services in the PAT object, find any video programs (with a pcr) return the associated PMT.
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[in]   int *e - used internally to enumerate objects. Pass 0 value int on first call then don't modify afterwards
 * @param[out]  struct ltntstools_pmt_s **pmt - ptr to the pmt object containing in the PAT
 * @return      0 - Success, PMT and PID contain details. < 0, no nore SCTE35 sevices, or error.
 */
int ltntstools_pat_enum_services_video(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmt);

/**
 * @brief       Enumerate all services in the PAT object, find any audio programs (with a pcr) return the associated PMT.
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[in]   int *e - used internally to enumerate objects. Pass 0 value int on first call then don't modify afterwards
 * @param[out]  struct ltntstools_pmt_s **pmt - ptr to the pmt object containing in the PAT
 * @param[out]  uint32_t **stream_type_array - ptr to the stream type array
 * @param[out]  uint16_t **pid_array - ptr to the pid array
 * @param[out]  int *pid_count - ptr to the pid count
 * @return      0 - Success, PMT and PID contain details. < 0, no nore SCTE35 sevices, or error.
 */
int ltntstools_pat_enum_services_audio(struct ltntstools_pat_s *pat, int *e, struct ltntstools_pmt_s **pmt, uint32_t **stream_type_array, uint16_t **pid_array, int *pid_count);

/**
 * @brief       Enumerate all services in the PAT object, return the associated PMT.
 * @param[in]   struct ltntstools_pat_s *pat - object
 * @param[in]   int *e - used internally to enumerate objects. Pass 0 value int on first call then don't modify afterwards
 * @param[out]  struct ltntstools_pmt_s **pmt - ptr to the pmt object containing in the PAT
 * @return      0 - Success, PMT and PID contain details. < 0, no nore SCTE35 sevices, or error.
 */
int ltntstools_pat_enum_services(struct ltntstools_pat_s *pat, int *e, uint16_t pid, struct ltntstools_pmt_s **pmt);

#ifdef __cplusplus
};
#endif

int ltntstools_pmt_query_video_pid(struct ltntstools_pmt_s *pmt, uint16_t *pid, uint8_t *estype);

/**
 * @brief      Finds audio pids.
 * @param[in]  pmt - the pmt entry.
 * @param[out] pid_count - a pointer to the count of discovered audio pids.
 * @return     An array[pid_count] of pmt_entry_t pointers. only the top-level pointer should be freed. Returns null on failure.
 */
const struct ltntstools_pmt_entry_s **ltntstools_pmt_enum_services_audio(const struct ltntstools_pmt_s *pmt, int *pid_count);

/**
 * @brief       Look at the stream-type AND descriptors to determine if this is an audio pid.
 * @param[in]   uint8_t esPayloadType - pmt elementary stream type.
 * @return      - 0  if not audio
 *		- 1  if the stream_type indicates the codec
 *		- the descriptor tag of the matching audio descriptor (>1)
 * @see        ltntstools_is_ESPayloadType_Audio()
 */
int ltntstools_pmt_entry_is_audio(const struct ltntstools_pmt_entry_s *pmt);

#endif /* TR101290_PAT_H */
