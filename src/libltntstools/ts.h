#ifndef TS_H
#define TS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SCR_TICKS_TO_MS(t) ((t) / 27000)
#define PTS_TICKS_TO_MS(t) ((t) / 90)

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

__inline__ uint8_t ltntstools_continuity_counter(const uint8_t *pkt)
{
	return *(pkt + 3) & 0x0f;
}

int ltntstools_scr(uint8_t *pkt, uint64_t *scr);

/* -1 if not found, else return byte index into pkt where 0000001 begins. */
int ltntstools_contains_pes_header(uint8_t *pkt, int byteCount);

#endif /* TS_H */
