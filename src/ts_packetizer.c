/*
 * Copyright (c) 2016 Kernel Labs Inc. All Rights Reserved
 *
 * Address: Kernel Labs Inc., PO Box 745, St James, NY. 11780
 * Contact: sales@kernellabs.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libltntstools/ts_packetizer.h"

/* Convert PES data into a series of TS packets */
int ltntstools_ts_packetizer(const uint8_t *buf, unsigned int byteCount,
	uint8_t **pkts, uint32_t *packetCount,
	int packetSize, uint8_t *cc, uint16_t pid)
{
	if ((!buf) || (byteCount == 0) || (!pkts) || (!packetCount) || (packetSize != 188) || (!cc) || (pid > 0x1fff))
		return -1;

	unsigned int offset = 0;

	int max = packetSize - 4;
	int packets = ((byteCount / max) + 1) * packetSize;
	int cnt = 0;

	uint8_t *arr = calloc(packets, packetSize);

	unsigned int rem = byteCount - offset;
	while (rem) {
		if (rem > max)
			rem = max;

		uint8_t *p = arr + (cnt * packetSize);
		*(p + 0) = 0x47;
		*(p + 1) = pid >> 8;
		*(p + 2) = pid;
		*(p + 3) = 0x10 | ((*cc) & 0x0f);
		memcpy(p + 4, buf + offset, rem);
		memset(p + 4 + rem, 0xff, packetSize - rem - 4);
		offset += rem;
		(*cc)++;

		if (cnt++ == 0)
			*(p + 1) |= 0x40; /* PES Header */

		rem = byteCount - offset;
	}

	*pkts = arr;
	*packetCount = cnt;
	return 0;
}

int ltntstools_ts_packetizer_with_pcr(const uint8_t *buf, unsigned int byteCount,
	uint8_t **pkts, uint32_t *packetCount,
	int packetSize, uint8_t *cc, uint16_t pid, int64_t pcr)
{
	if (!buf || byteCount == 0 || !pkts || !packetCount || packetSize != 188 || !cc || pid > 0x1fff)
		return -1;

	/* Worst case: one extra packet due to PCR/adaptation overhead */
	unsigned int maxPackets = ((byteCount + 183) / 184) + 2;
	uint8_t *arr = calloc(maxPackets, packetSize);
	if (!arr)
		return -1;

	unsigned int pos = 0;
	uint32_t cnt = 0;

	while (pos < byteCount) {
		uint8_t *p = arr + (cnt * packetSize);
		memset(p, 0xff, packetSize);

		p[0] = 0x47;
		p[1] = (pid >> 8) & 0x1f;
		p[2] = pid & 0xff;
		p[3] = (*cc) & 0x0f; /* continuity counter */

		if (cnt == 0)
			p[1] |= 0x40; /* PUSI */

		int needPCR = (pcr != -1 && cnt == 0) ? 1 : 0;

		/* Start by assuming payload only */
		int payload_offset = 4;
		int payload_capacity = 184;

		if (needPCR) {
			/* We know adaptation field is required for PCR */
			payload_offset += 8;
			payload_capacity -= 8;
		}

		unsigned int rem = byteCount - pos;
		int cpy = rem < (unsigned int)payload_capacity ? (int)rem : payload_capacity;

		/* If packet is not completely filled, we need adaptation stuffing */
		int stuffing = payload_capacity - cpy;

		if (needPCR || stuffing > 0) {
			p[3] |= 0x30; /* adaptation + payload */

			if (needPCR) {
				/* adaptation_field_length excludes itself */
				int afl = 7 + stuffing;
				p[4] = afl;
				p[5] = 0x10; /* PCR flag */

				uint64_t base = pcr / 300;
				uint64_t ext = pcr % 300;
				p[6]  = (base >> 25) & 0xff;
				p[7]  = (base >> 17) & 0xff;
				p[8]  = (base >> 9) & 0xff;
				p[9]  = (base >> 1) & 0xff;
				p[10] = ((base & 0x1) << 7) | 0x7e | ((ext >> 8) & 0x01);
				p[11] = ext & 0xff;

				/* stuffing bytes follow PCR */
				for (int i = 0; i < stuffing; i++)
					p[12 + i] = 0xff;

				payload_offset = 12 + stuffing;
			} else {
				/* adaptation only for stuffing */
				int afl = stuffing - 1;
				if (stuffing == 1) {
					/* single adaptation byte: length=0, no flags */
					p[4] = 0;
					payload_offset = 5;
				} else {
					p[4] = afl;
					p[5] = 0x00; /* no flags */
					for (int i = 0; i < stuffing - 1; i++)
						p[6 + i] = 0xff;
					payload_offset = 4 + 1 + stuffing;
				}
			}
		} else {
			p[3] |= 0x10; /* payload only */
			payload_offset = 4;
		}

		memcpy(p + payload_offset, buf + pos, cpy);
		pos += cpy;
		(*cc)++;
		cnt++;
	}

	*pkts = arr;
	*packetCount = cnt;
	return 0;
}