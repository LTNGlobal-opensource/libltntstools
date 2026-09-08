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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libltntstools/ts_packetizer.h"
#include "libltntstools/crc32.h"

/* Convert PES data into a series of TS packets */
int ltntstools_ts_packetizer(const uint8_t *buf, unsigned int byteCount,
	uint8_t **pkts, uint32_t *packetCount,
	int packetSize, uint8_t *cc, uint16_t pid)
{
	if ((!buf) || (byteCount == 0) || (!pkts) || (!packetCount) || (packetSize != 188) || (!cc) || (pid > 0x1fff))
		return -1;

	unsigned int offset = 0;

	int max = packetSize - 4;
	int packets = (byteCount / max) + 1;
	int cnt = 0;

	uint8_t *arr = calloc(packets, packetSize);
	if (!arr)
		return -1;

	unsigned int rem = byteCount - offset;
	while (rem) {
		int shortPacket = (rem < (unsigned int)max);
		if (rem > (unsigned int)max)
			rem = max;

		uint8_t *p = arr + (cnt * packetSize);
		*(p + 0) = 0x47;
		*(p + 1) = pid >> 8;
		*(p + 2) = pid;

		int payload_offset = 4;
		if (shortPacket) {
			/* Not enough payload to fill this packet. Use a proper
			 * adaptation field to stuff the remainder, rather than
			 * appending raw padding bytes to a payload-only packet
			 * (which a compliant decoder would misread as ES data).
			 */
			int adaptation_field_length = (packetSize - 4 - 1) - rem;
			*(p + 3) = 0x30 | ((*cc) & 0x0f); /* adaptation + payload */
			*(p + 4) = adaptation_field_length;
			payload_offset = 4 + 1 + adaptation_field_length;
			if (adaptation_field_length > 0) {
				*(p + 5) = 0x00; /* no adaptation flags */
				memset(p + 6, 0xff, adaptation_field_length - 1);
			}
		} else {
			*(p + 3) = 0x10 | ((*cc) & 0x0f); /* payload only */
		}

		memcpy(p + payload_offset, buf + offset, rem);
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

/**
 * Build a single-service DVB SDT (actual_transport_stream, table_id 0x42) transport
 * packet, containing exactly one service_descriptor (tag 0x48) built from providerName
 * and serviceName. Follows the same "one packet only" convention as
 * ltntstools_pat_create_packet_ts()/ltntstools_pmt_create_packet_ts(): fails if the
 * section doesn't fit in a single 188 byte packet.
 *
 * transport_stream_id, original_network_id and service_id aren't exposed on the
 * bitrate_smoother command line (only provider/service name are) -- callers needing
 * different values should hardcode them here.
 */
int ltntstools_ts_packetizer_build_sdt(const char *providerName, const char *serviceName,
	uint16_t transport_stream_id, uint16_t original_network_id, uint16_t service_id,
	uint8_t cc, uint8_t *packet, int packetLength)
{
	if (!providerName || !serviceName || !packet || packetLength != 188)
		return -1;

	size_t providerLen = strlen(providerName);
	size_t serviceLen = strlen(serviceName);
	if (providerLen > 255 || serviceLen > 255)
		return -1;

	int descLen = 3 + (int)providerLen + (int)serviceLen; /* service_type(1) + 2 length bytes + strings */
	int loopLen = 2 + descLen;                             /* descriptor_tag(1) + descriptor_length(1) + descLen */
	int sectionLen = 17 + loopLen;                          /* 8 fixed fields + 5 service fields + loopLen + 4 CRC */

	int contentBytes = 8 + sectionLen; /* 5 byte TS header/pointer + table_id(1) + length(2) + sectionLen */
	if (contentBytes > packetLength) {
		fprintf(stderr, "%s() provider/service names need %d bytes, which does not fit in a single %d byte packet\n",
			__func__, contentBytes, packetLength);
		return -1;
	}

	uint8_t *p = packet;
	int i = 0;

	memset(p, 0xFF, packetLength);

#define SDT_PID 0x0011
	p[i++] = 0x47;
	p[i++] = 0x40 | ((SDT_PID >> 8) & 0x1f);
	p[i++] = SDT_PID & 0xff;
	p[i++] = 0x10 | (cc & 0x0f);
	p[i++] = 0x00; /* pointer field */

	p[i++] = 0x42; /* table_id: service_description_section, actual_transport_stream */
	p[i++] = 0xF0 | ((sectionLen >> 8) & 0x0f); /* section_syntax_indicator=1, reserved_future_use=1, reserved=11 */
	p[i++] = sectionLen & 0xff;

	p[i++] = transport_stream_id >> 8;
	p[i++] = transport_stream_id & 0xff;

	p[i++] = 0xC1; /* reserved(2)=11, version_number(5)=0, current_next_indicator(1)=1 */
	p[i++] = 0x00; /* section_number */
	p[i++] = 0x00; /* last_section_number */

	p[i++] = original_network_id >> 8;
	p[i++] = original_network_id & 0xff;

	p[i++] = 0xFF; /* reserved_future_use */

	p[i++] = service_id >> 8;
	p[i++] = service_id & 0xff;
	p[i++] = 0xFC; /* reserved_future_use(6)=111111, EIT_schedule_flag=0, EIT_present_following_flag=0 */

	uint8_t running_status = 4; /* running */
	uint8_t free_ca_mode = 0;   /* not scrambled */
	p[i++] = (running_status << 5) | (free_ca_mode << 4) | ((loopLen >> 8) & 0x0f);
	p[i++] = loopLen & 0xff;

	p[i++] = 0x48; /* descriptor_tag: service_descriptor */
	p[i++] = descLen;
	p[i++] = 0x01; /* service_type: digital television service */
	p[i++] = (uint8_t)providerLen;
	memcpy(&p[i], providerName, providerLen);
	i += providerLen;
	p[i++] = (uint8_t)serviceLen;
	memcpy(&p[i], serviceName, serviceLen);
	i += serviceLen;

	uint32_t crc;
	ltntstools_getCRC32(&p[5], i - 5, &crc);

	p[i++] = (crc >> 24) & 0xff;
	p[i++] = (crc >> 16) & 0xff;
	p[i++] = (crc >> 8) & 0xff;
	p[i++] = crc & 0xff;

	return 0;
}
