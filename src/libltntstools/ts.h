#ifndef TS_H
#define TS_H

/**
 * @file        ts.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       Helper functions to query and inspect ISO138-1 MPEG-TS transport packets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SCR_TICKS_TO_MS(t) ((t) / 27000)
#define PTS_TICKS_TO_MS(t) ((t) / 90)

#define TSTOOLS_PID_PAT 0
#define TSTOOLS_TABLEID_PAT 0

#define MAX_SCR_VALUE 2576980377600
#define MAX_PTS_VALUE 8589934591

#define PCR_CLOCK_HZ 27000000

/** Conventions:
 * 
 * All clocks are expressed as int64_t
 *    TODO: those expressed currently as uint64_t need to be adjusted.
 *
 * Transport packet ID's are uint16_t
 * 
 * The argument 'pkt' assumes one or more aligned transport packets in a buffer.
 * 
 * The argument 'buf' as it relates to transport packets, means it's a buffer
 * of aligned OR unaligned packets, or just a general buffer of NALS, ES type data.
 * 
 * Buffers of bytes are uint8_t, no exceptions.
 * 
 * Buffers that are not expected to change are const typed, no exceptions.
 */

/**
 * @brief       For a given transport packet, check if the sync byte is present.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      Boolean. 1 on success else 0.
 */
static inline int ltntstools_sync_present(const uint8_t *pkt)
{
	return *pkt == 0x47;
}

/**
 * @brief       For a given transport packet, check if the Transport Error Indicator is set.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      Boolean. 1 on success else 0.
 */
static inline int ltntstools_tei_set(const uint8_t *pkt)
{
	return *(pkt + 1) & 0x80 ? 1 : 0;
}

/**
 * @brief       For a given transport packet, check if the Payload Unit Start Indicator is set.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      Boolean. 1 on success else 0.
 */
static inline int ltntstools_payload_unit_start_indicator(const uint8_t *pkt)
{
	return *(pkt + 1) & 0x40 ? 1 : 0;
}

/**
 * @brief       For a given transport packet, check if the Transport Priority Bit is set.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      Boolean. 1 on success else 0.
 */
static inline int ltntstools_transport_priority(const uint8_t *pkt)
{
	return *(pkt + 1) & 0x20 ? 1 : 0;
}

/**
 * @brief       For a given transport packet, query the pid (Packet Identifier)
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      PID
 */
static inline uint16_t ltntstools_pid(const uint8_t *pkt)
{
	uint16_t pid = (*(pkt + 1) << 8 ) | *(pkt + 2);
	return pid & 0x1fff;
}

/**
 * @brief       For a given transport packet, query the 2-but transport scrambling control field.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      value
 */
static inline uint8_t ltntstools_transport_scrambling_control(const uint8_t *pkt)
{
	return *(pkt + 3) >> 6;
}

/**
 * @brief       For a given transport packet, query the transport scrambling control field.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      two bit field.
 */
static inline uint8_t ltntstools_adaption_field_control(const uint8_t *pkt)
{
	return (*(pkt + 3) >> 4) & 0x03;
}

/**
 * @brief       For a given transport packet, check if the adaption flag is set.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      Boolean. 1 on success else 0.
 */
static inline unsigned int ltntstools_has_adaption(const uint8_t *pkt)
{
        unsigned char v = (*(pkt + 3) >> 4) & 0x03;
        if ((v == 2) || (v == 3))
                return 1;

        return 0;
}

/**
 * @brief       For a given transport packet, assuming the adaption field is present,
 *              see ltntstools_has_adaption() before calling this, return the 
 *              8-bit field length value.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      eight bit field.
 */
static inline uint8_t ltntstools_adaption_field_length(const uint8_t *pkt)
{
	return *(pkt + 4);
}

/**
 * @brief       For a given transport packet, query the continuity counter field.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      four bit field.
 */
static inline uint8_t ltntstools_continuity_counter(const uint8_t *pkt)
{
	return *(pkt + 3) & 0x0f;
}

/**
 * @brief       Write a PCR value into a byte array conformant to the PCR field in MPEG-TS header (48-bit base+reserved+ext)
 * @param[out]  uint8_t *dst - Pointer to insert into (e.g. "pkt + 6")
 * @param[in]   int lengthBytes - Byte range of dst to not exceed
 * @param[in]   uint64_t pcr - PCR value to write into dst
 */
void ltntstools_pcr_packTo(uint8_t *dst, int lengthBytes, uint64_t pcr);

/**
 * @brief       In a SCR/PCR clock, subtract 'from' from 'to', compensate for a clock
 *              wrap and return a positive number of ticks.
 * @param[in]   int64_t from - tick value
 * @param[in]   int64_t to - tick value
 * @return      int64_t - Always returns a positive number of ticks in the 27MHz clock
 */
static inline int64_t ltntstools_scr_diff(int64_t from, int64_t to)
{
	int64_t diffTicks;

	if ((from > to) && (to < (5 * 27000000))) {
		/* Probably we wrapped, or the stream restarted. */
		diffTicks = MAX_SCR_VALUE - from;
		diffTicks += to;
	} else {
		diffTicks = to - from;
	}

	return diffTicks;
}

/**
 * @brief       Add a positive or negative number of ticks to a SCR/PCR clock, compensate for a clock wrap.
 * @param[in]   int64_t scr - tick value
 * @param[in]   int64_t ticks - tick value
 * @return      int64_t - Always returns a positive number of ticks in the 27MHz clock
 */
static inline int64_t ltntstools_scr_add(int64_t scr, int64_t ticks)
{
	int64_t clock = scr + ticks;

	if (clock > MAX_SCR_VALUE)
		clock -= MAX_SCR_VALUE;
	else
	if (clock < 0)
		clock += MAX_SCR_VALUE;
	
	return clock;
}

/* Always returns a positive number of ticks in the 90MHz clock */
static inline int64_t ltntstools_pts_diff(int64_t from, int64_t to)
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

/**
 * @brief       Helper function. For a given transport packet, return the PCR value contained
 *              in the packet, if a PCR is detected and present.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @param[in]   uint64_t *scr - PCR contained in packet.
 * @return      0 on success, < 0 on error.
 */
int ltntstools_scr(const uint8_t *pkt, uint64_t *scr);

/**
 * @brief       Search buffer (forwards) for the byte sequence 000001, a PES header signature.
 * @param[in]   const uint8_t *buf - Buffer of data, possibly containing a TS packet or a PES packet.
 * @param[in]   int lengthBytes - Buffer length in bytes.
 * @return      >= 0 - Success, return byte index into buf where 0000001 begins.
 * @return      < 0 - Error
 */
int ltntstools_contains_pes_header(const uint8_t *buf, int lengthBytes);

/**
 * @brief       Search buffer (reverse) for the byte sequence 000001, a PES header signature.
 * @param[in]   const uint8_t *buf - Buffer of data, possibly containing a TS packet or a PES packet.
 * @param[in]   int lengthBytes - Buffer length in bytes.
 * @return      >= 0 - Success, return byte index into buf where 0000001 begins.
 * @return      < 0 - Error
 */
int ltntstools_contains_pes_header_reverse(const uint8_t *buf, int lengthBytes);

/**
 * @brief       For a given transport packet, assume the packet contains a table
 *              and extract the section table identifier.
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      eight bit field.
 */
unsigned int ltntstools_get_section_tableid(const uint8_t *pkt);

/**
 * @brief       For a PMT streamType field, lookup a friendly text description.
 *              The function will never return NULL, a string will be returned for every possible input.
 *              Eg. "ISO/IEC 11172 Video"
 * @param[in]   const uint8_t *pkt - Transport packet.
 * @return      const char * - Description
 */
const char *ltntstools_GetESPayloadTypeDescription(uint8_t esPayloadType);

/**
 * @brief       For a PMT streamType field, if the payload is deemed video, return 1 true else 0 false.
 * @param[in]   uint8_t esPayloadType - pmt elementary stream type.
 * @return      Boolean.
 */
int ltntstools_is_ESPayloadType_Video(uint8_t esPayloadType);

/**
 * @brief       For a PMT streamType field, if the payload is deemed audio, return 1 true else 0 false.
 * @param[in]   uint8_t esPayloadType - pmt elementary stream type.
 * @return      Boolean.
 * @see ltntstools_pmt_entry_is_audio()
 */
int ltntstools_is_ESPayloadType_Audio(uint8_t esPayloadType);

/**
 * @brief       For a given buffer atleast 188 bytes long, create a null padding packet.
 * @param[in]   uint8_t *pkt - Destination buffer.
 */
void ltntstools_generateNullPacket(uint8_t *pkt);

/**
 * @brief       For a buffer of data, a minimum of 3*188 bytes long, find the position in the buffer
 *              where unaligned transport packets become aligned, with three consecutive sync bytes.
 * @param[in]   const uint8_t *buf - buffer of bytes, possibly transport packets, probably not aligned.
 * @param[in]   int lengthBytes - length of buffer in bytes.
 * @return      int - index position in the buffer, else -1 if no sync found.
 */
int ltntstools_findSyncPosition(const uint8_t *buf, int lengthBytes);

/**
 * @brief       Enumerator struct used with ltntstools_queryPCR_pid()
 */
struct ltntstools_pcr_position_s
{
	int64_t  pcr;
	uint64_t offset;
	uint16_t pid;
};
/**
 * @brief       For an existing array, add an additional element.
 */
int ltntstools_pcr_position_append(struct ltntstools_pcr_position_s **array, int *arrayLength, struct ltntstools_pcr_position_s *p);

/**
 * @brief       Enumerator function to assist with using ltntstools_queryPCR_pid()
 */
static inline void ltntstools_pcr_position_reset(struct ltntstools_pcr_position_s *p)
{
	p->pcr = -1;
	p->offset = 0;
	p->pid = 0;
};

/**
 * @brief       For a buffer of data, which don't need to be packet aligned, containing any number of pids,
 *              find all of the available PCRs across all pids, along with index positions.
 *              The caller is responsible for the lifespan of the resulting array.
 *              If the packets contain RTP headers (12 bytes), they're automatically skipped.
 * @param[in]   const uint8_t *buf - buffer of bytes, possibly transport packets, probably not aligned.
 * @param[in]   int lengthBytes - length of buffer in bytes.
 * @param[in]   uint64_t addr - deprecated, don't use.
 * @param[out]  struct ltntstools_pcr_position_s **array - length of buffer in bytes.
 * @param[in]   int *arrayLength - number of elements in the returned array.
 * @return      0 on success else < 0.
 */
int ltntstools_queryPCRs(const uint8_t *buf, int lengthBytes, uint64_t addr, struct ltntstools_pcr_position_s **array, int *arrayLength);

/**
 * @brief       For a buffer of data, which don't need to be packet aligned, containing any number of pids,
 *              find the next available PCRs for a single pid, along with index positions.
 *              It's more efficient if you pass pktALigned if you know this in advance.
 *              If the packets contain RTP headers (12 bytes), they're automatically skipped.
 * @param[in]   const uint8_t *buf - buffer of bytes, possibly transport packets, probably not aligned.
 * @param[in]   int lengthBytes - length of buffer in bytes.
 * @param[in]   struct ltntstools_pcr_position_s *pos - enumerator
 * @param[in]   uint16_t pcrPID - transport packet identifier
 * @param[in]   int pktAligned - boolean. Signal to the API if the buffer contains fully aligned transport packets (or not).
 * @return      0 on success else < 0.
 */
int ltntstools_queryPCR_pid(const uint8_t *buf, int lengthBytes, struct ltntstools_pcr_position_s *pos, uint16_t pcrPID, int pktAligned);

/**
 * @brief       Generate a fully formed legal packet containing a PCR structure.
 *              This is generally used by test tools that want to downstream inspect PCRs.
 *              The 'pkt' buffer needs to be 188 bytes long.
 * @param[in]   uint8_t *pkt - destination buffer.
 * @param[in]   int lengthBytes - length of buffer in bytes.
 * @param[in]   uint16_t pid - transport packet identifier
 * @param[in]   uint8_t *cc - Use and update the continuity counter pointer.
 * @param[in]   uint64_t pcr- Clock value that will be written into the packet.
 * @return      0 on success else < 0.
 */
int ltntstools_generatePCROnlyPacket(uint8_t *pkt, int lengthBytes, uint16_t pid, uint8_t *cc, uint64_t pcr);


/**
 * @brief       One of three functions that are used by tools to generate and validate transport packet content,
 *              as being bit-for-bit perfect after move the packets through a transport medium.
 *              The ltntstools_generatePacketWith64bCounter() function will generate a new packet
 *              with correct CC counter information to pass any TR101290 test.
 *              The ltntstools_updatePacketWith64bCounter() function will modify a previously generated new packet
 *              with correct CC counter information to pass any TR101290 test, its significantly more cpu efficient for
 *              second and subsequent packet updates.
 *              The ltntstools_verifyPacketWith64bCounter() function is the downstream verification function
 *              used to check that no bits have been flipped or data lost.
 *              This is generally used by test tools that want to downstream inspect PCRs.
 *              The 'pkt' buffer needs to be 188 bytes long.
 * @param[in]   uint8_t *pkt - destination buffer.
 * @param[in]   int lengthBytes - length of buffer in bytes.
 * @param[in]   uint16_t pid - transport packet identifier
 * @param[in]   uint8_t *cc - Use and update the continuity counter pointer.
 * @param[in]   uint64_t counter - a user specific count value (increment by 1) so packets are sequenced correctly.
 * @return      0 on success else < 0.
 */
int ltntstools_generatePacketWith64bCounter(uint8_t *pkt, int lengthBytes, uint16_t pid, uint8_t *cc, uint64_t counter);

int ltntstools_updatePacketWith64bCounter(unsigned char *pkt, int lengthBytes, uint16_t pid, uint8_t *cc, uint64_t counter);

/**
 * @brief       One of two functions that are used by tools to generate and validate transport packet content,
 *              as being bit-for-bit perfect after move the packets through a transport medium.
 *              The ltntstools_generatePacketWith64bCounter() function will generate a new packet
 *              with correct CC counter information to pass any TR101290 test.
 *              The ltntstools_verifyPacketWith64bCounter() function is the downstream verification function
 *              used to check that no bits have been flipped or data lost.
 *              This is generally used by test tools that want to downstream inspect PCRs.
 *              The 'pkt' buffer needs to be 188 bytes long.
 * @param[in]   uint8_t *pkt - destination buffer.
 * @param[in]   int lengthBytes - length of buffer in bytes.
 * @param[in]   uint16_t pid - transport packet identifier
 * @param[in]   uint64_t lastCounter - The previous 'currentCounter' so discontinuities can be measured.
 * @param[in]   uint64_t *currentCounter - a user specific count value (increment by 1) so packets are sequenced correctly.
 * @return      0 on success else < 0 indicating fault/damage to the packet.
 */
int ltntstools_verifyPacketWith64bCounter(uint8_t *pkt, int lengthBytes, uint16_t pid, uint64_t lastCounter, uint64_t *currentCounter);

/**
 * @brief       File a given mpegts CBR transport file. Determine the overall SPTS/MPTS bitrate quickly and efficiently.
 * @param[in]   const char *filename - input filename
 * @param[out]  uint32_t *bps - Calculate bps
 * @return      0 on success else < 0 indicating fault/damage to the packet.
 */
int ltntstools_file_estimate_bitrate(const char *filename, uint32_t *bps);

/**
 * @brief       Convert a pcr into a human readibly string. Either pass a buffer of atleast 16 bytes,
 *              or pass NULL and a buffer will be allocated for you. If you pass NULL, you own the lifespan, don't
 *              leak it.
 * @param[out]  char **buf
 * @param[in]   int64_t pcr - tick value
 */
void ltntstools_pcr_to_ascii(char **buf, int64_t pcr);

/**
 * @brief       Convert a pts into a human readibly string. Either pass a buffer of atleast 16 bytes,
 *              or pass NULL and a buffer will be allocated for you. If you pass NULL, you own the lifespan, don't
 *              leak it.
 * @param[out]  char **buf
 * @param[in]   int64_t pts - tick value
 */
void ltntstools_pts_to_ascii(char **buf, int64_t pts);

#endif /* TS_H */
