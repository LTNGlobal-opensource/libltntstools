#ifndef TS_H
#define TS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SCR_TICKS_TO_MS(t) ((t) / 27000)
#define PTS_TICKS_TO_MS(t) ((t) / 90)

#define TSTOOLS_PID_PAT 0

#define MAX_SCR_VALUE 2576980377600
#define MAX_PTS_VALUE 8589934591

__inline__ int ltntstools_sync_present(const uint8_t *pkt)
{
	return *pkt == 0x47;
}

__inline__ int ltntstools_tei_set(const uint8_t *pkt)
{
	return *(pkt + 1) & 0x80 ? 1 : 0;
}

__inline__ int ltntstools_payload_unit_start_indicator(const uint8_t *pkt)
{
	return *(pkt + 1) & 0x40 ? 1 : 0;
}

__inline__ int ltntstools_transport_priority(const uint8_t *pkt)
{
	return *(pkt + 1) & 0x20 ? 1 : 0;
}

__inline__ uint16_t ltntstools_pid(const uint8_t *pkt)
{
	uint16_t pid = (*(pkt + 1) << 8 ) | *(pkt + 2);
	return pid & 0x1fff;
}

__inline__ uint8_t ltntstools_transport_scrambling_control(const uint8_t *pkt)
{
	return *(pkt + 3) >> 6;
}

__inline__ uint8_t ltntstools_adaption_field_control(const uint8_t *pkt)
{
	return (*(pkt + 3) >> 4) & 0x03;
}

__inline__ unsigned int ltntstools_has_adaption(unsigned char *pkt)
{
        unsigned char v = (*(pkt + 3) >> 4) & 0x03;
        if ((v == 2) || (v == 3))
                return 1;

        return 0;
}

__inline__ uint8_t ltntstools_adaption_field_length(const uint8_t *pkt)
{
	return *(pkt + 4);
}

__inline__ uint8_t ltntstools_continuity_counter(const uint8_t *pkt)
{
	return *(pkt + 3) & 0x0f;
}

/* Always returns a positive number of ticks in the 27MHz clock */
__inline__ int64_t ltntstools_scr_diff(int64_t from, int64_t to)
{
	int64_t diffTicks;

	if (from > to) {
		/* Probably we wrapped, or the stream restarted. */
		diffTicks = MAX_SCR_VALUE - from;
		diffTicks += to;
	} else {
		diffTicks = to - from;
	}

	return diffTicks;
}

/* Always returns a positive number of ticks in the 90MHz clock */
__inline__ int64_t ltntstools_pts_diff(int64_t from, int64_t to)
{
	int64_t diffTicks;

	if (from > to) {
		/* Probably we wrapped, or the stream restarted. */
		diffTicks = MAX_PTS_VALUE - from;
		diffTicks += to;
	} else {
		diffTicks = to - from;
	}

	return diffTicks;
}

int ltntstools_scr(uint8_t *pkt, uint64_t *scr);

/**
 * @brief       Search buffer for the byte sequence 000001, a PES header signature.
 * @param[in]   uint8_t *buf - Buffer of data, possibly containing a TS packet or a PES packet.
 * @param[in]   int lengthBytes - Buffer length in bytes.
 * @return      >= 0 - Success, return byte index into buf where 0000001 begins.
 * @return      < 0 - Error
 */
int ltntstools_contains_pes_header(uint8_t *buf, int lengthBytes);
int ltntstools_contains_pes_header_reverse(uint8_t *buf, int lengthBytes);

unsigned int ltntstools_get_section_tableid(unsigned char *pkt);

const char *ltntstools_GetESPayloadTypeDescription(unsigned char esPayloadType);

void ltntstools_generateNullPacket(unsigned char *pkt);

int ltntstools_findSyncPosition(const uint8_t *buf, int lengthBytes);

struct ltntstools_pcr_position_s
{
	int64_t  pcr;
	uint64_t offset;
	uint16_t pid;
};

__inline__ void ltntstools_pcr_position_reset(struct ltntstools_pcr_position_s *p)
{
	p->pcr = -1;
	p->offset = 0;
	p->pid = 0;
};

/* Query a buffer containing transport packets from many pids,
 * Return all of the PCRs found for any pids.
 */
int ltntstools_queryPCRs(const uint8_t *buf, int lengthBytes, uint64_t addr, struct ltntstools_pcr_position_s **array, int *arrayLength);

/* Query a buffer containing transport packets from many pids,
 * Return the first PCR found for pcrPID.
 */
int ltntstools_queryPCR_pid(const uint8_t *buf, int lengthBytes, struct ltntstools_pcr_position_s *pos, uint16_t pcrPID, int pktAligned);

int ltntstools_generatePCROnlyPacket(uint8_t *pkt, int lengthBytes, uint16_t pid, uint8_t *cc, uint64_t pcr);
int ltntstools_generatePacketWith64bCounter(unsigned char *pkt, int lengthBytes, uint16_t pid, uint8_t *cc, uint64_t counter);
int ltntstools_verifyPacketWith64bCounter(unsigned char *pkt, int lengthBytes, uint16_t pid, uint64_t lastCounter, uint64_t *currentCounter);

#endif /* TS_H */
