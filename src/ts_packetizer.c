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
	int packetSize, uint8_t *cc, uint16_t pid, int64_t pcr, int random_access_indicator,
	int elementary_stream_priority_indicator)
{
	if (!buf || byteCount == 0 || !pkts || !packetCount || packetSize != 188 || !cc || pid > 0x1fff)
		return -1;

	/* Conservative sizing:
	 * first packet may carry PCR and/or adaptation flags, reducing payload capacity
	 */
	unsigned int maxPackets = (byteCount + 175) / 176 + 2;
	uint8_t *arr = calloc(maxPackets, packetSize);
	if (!arr)
		return -1;

	unsigned int pos = 0;
	uint32_t cnt = 0;

	while (pos < byteCount) {
		uint8_t *p = arr + (cnt * packetSize);
		memset(p, 0xff, packetSize);

		int firstPacket = (cnt == 0);
		int needPCR = (pcr != -1 && firstPacket);
		int needIndicators = firstPacket &&
			(random_access_indicator || elementary_stream_priority_indicator);

		uint8_t af_flags = 0x00;
		if (needPCR)
			af_flags |= 0x10; /* PCR_flag */
		if (firstPacket && random_access_indicator)
			af_flags |= 0x40;
		if (firstPacket && elementary_stream_priority_indicator)
			af_flags |= 0x20;

		p[0] = 0x47;
		p[1] = ((pid >> 8) & 0x1f);
		p[2] = pid & 0xff;
		p[3] = (*cc) & 0x0f;

		if (firstPacket)
			p[1] |= 0x40; /* PUSI */

		unsigned int rem = byteCount - pos;
		int payload_offset = 4;
		int cpy = 0;

		if (needPCR) {
			/* adaptation + payload */
			p[3] |= 0x30;

			if (rem >= 176) {
				/* exact PCR packet, no stuffing */
				p[4] = 7;          /* flags + PCR */
				p[5] = af_flags;
				cpy = 176;
				payload_offset = 12;
			} else {
				/* short packet with PCR and stuffing */
				int adaptation_field_length = 183 - (int)rem;
				p[4] = adaptation_field_length;
				p[5] = af_flags;

				cpy = (int)rem;
				payload_offset = 4 + 1 + adaptation_field_length;

				/* stuffing after PCR starts at p[12] */
				for (int i = 12; i < payload_offset; i++)
					p[i] = 0xff;
			}

			{
				uint64_t base = (uint64_t)(pcr / 300);
				uint64_t ext  = (uint64_t)(pcr % 300);
				p[6]  = (base >> 25) & 0xff;
				p[7]  = (base >> 17) & 0xff;
				p[8]  = (base >> 9)  & 0xff;
				p[9]  = (base >> 1)  & 0xff;
				p[10] = ((base & 0x1) << 7) | 0x7e | ((ext >> 8) & 0x01);
				p[11] = ext & 0xff;
			}

		} else if (needIndicators) {
			/* No PCR, but need adaptation field for RAI / ES priority flags */
			p[3] |= 0x30;

			if (rem >= 182) {
				/* minimal adaptation field: length=1, flags only, no stuffing */
				p[4] = 1;
				p[5] = af_flags;
				cpy = 182;
				payload_offset = 6;
			} else {
				/* short packet with flags and stuffing */
				int adaptation_field_length = 183 - (int)rem;
				p[4] = adaptation_field_length;
				p[5] = af_flags;

				cpy = (int)rem;
				payload_offset = 4 + 1 + adaptation_field_length;

				/* stuffing begins after flags byte */
				for (int i = 6; i < payload_offset; i++)
					p[i] = 0xff;
			}

		} else {
			if (rem >= 184) {
				/* payload only */
				p[3] |= 0x10;
				cpy = 184;
				payload_offset = 4;
			} else {
				/* adaptation + payload with stuffing */
				int adaptation_field_length = 183 - (int)rem;
				p[3] |= 0x30;
				p[4] = adaptation_field_length;

				cpy = (int)rem;
				payload_offset = 4 + 1 + adaptation_field_length;

				if (adaptation_field_length > 0) {
					p[5] = 0x00; /* no flags */
					for (int i = 6; i < payload_offset; i++)
						p[i] = 0xff;
				}
			}
		}

		memcpy(p + payload_offset, buf + pos, cpy);
		pos += cpy;
		*cc = (*cc + 1) & 0x0f;
		cnt++;
	}

	*pkts = arr;
	*packetCount = cnt;
	return 0;
}