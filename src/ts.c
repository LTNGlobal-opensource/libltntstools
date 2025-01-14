#include "libltntstools/ts.h"
#include "libltntstools/streammodel.h"
#include <inttypes.h>

/* Convert a pcr base + extension field back into a 27MHz System Clock reference value.
 * Ptr points to a 6 byte raw data, which we'll parse.
 * The Adaption spec calls for the PCR to have the following format:
 * program_clock_reference_base       33bits
 * reserved                            6bits
 * program_clock_reference_extension   9bits
 */
uint64_t ltntstools_pcrToScr(const uint8_t *ptr, int len)
{
        uint64_t pcr_base;
        pcr_base  = (((uint64_t)*(ptr + 0)) << 25);
        pcr_base |= (((uint64_t)*(ptr + 1)) << 17);
        pcr_base |= (((uint64_t)*(ptr + 2)) <<  9);
        pcr_base |= (((uint64_t)*(ptr + 3)) <<  1);
        pcr_base |= (((uint64_t)*(ptr + 4)) >>  7);
        pcr_base &= 0x1ffffffffLL;

        uint64_t pcr_ext;
        pcr_ext  = ((uint64_t)*(ptr + 4)) << 8;
        pcr_ext |= ((uint64_t)*(ptr + 5));
        pcr_ext &= 0x1ff;

        return (pcr_base * 300) + pcr_ext;
}

int ltntstools_scr(const uint8_t *pkt, uint64_t *scr)
{
	if (ltntstools_sync_present(pkt) == 0)
		return -1;

	if (ltntstools_adaption_field_control(pkt) < 2)
		return -1;

	uint8_t adaption_field_length = *(pkt + 4);
	if (adaption_field_length == 0)
		return -1;

	/* Extract the PCR -- See ISO13818 Table 2.7 */

	/* Ensure PCR_flag is set. */
	if ((*(pkt + 5) & 0x10) == 0)
		return -1;

	uint64_t v = ltntstools_pcrToScr(pkt + 6, 6);
	*scr = v;

	return 0;
}

/* Helper function, packs the pcr into six bytes
 * ready for packing into an adaption field.
 */
void ltntstools_pcr_packTo(uint8_t *dst, int lengthBytes, uint64_t pcr)
{
	/* PCR / SCR value in 27MHz */
	/* The PCR is a 42bit number coded in two parts */

	/* Base is 90KHz clock, ext is 27MHz clock */
	uint64_t base = (pcr / 300);
	uint64_t ext = pcr & 0x1ff;

	*(dst + 0)  = base >> 25LL;
	*(dst + 1)  = base >> 17LL;
	*(dst + 2)  = base >>  9LL;
	*(dst + 3)  = base >>  1LL;
	*(dst + 4)  = base <<  7LL;
	*(dst + 4) |= 0x7e;
	*(dst + 4) |= (ext & 0x100LL) >> 8;
	*(dst + 5)  = ext;
}

int ltntstools_generatePCROnlyPacket(uint8_t *pkt, int lengthBytes, uint16_t pid, uint8_t *cc, uint64_t pcr)
{
	if (lengthBytes < 188 || !pkt || !cc)
		return -1;

	memset(pkt, 0xff, 188);

	*(pkt + 0) = 0x47;
	*(pkt + 1) = (pid & 0x1fff) >> 8;
	*(pkt + 2) = pid;
	*(pkt + 3) = 0x20 | ((*cc)++ & 0x0f); /* Adaption field only, no payload */
	*(pkt + 4) = 1 + 6; /* Adaption field length. Indicators plus pcr field */
	*(pkt + 5) = 0x10; /* PCR_flag = 1 */

	ltntstools_pcr_packTo(pkt + 6, 188 - 6, pcr);

	return 0;
}

int ltntstools_contains_pes_header_reverse(const uint8_t *buf, int lengthBytes)
{
	const char pattern[] = { 0x00, 0x00, 0x01 };
	for (int i = lengthBytes - 3; i >= 0; i--) {
		if (memcmp(buf + i, pattern, sizeof(pattern)) == 0)
			return i;
	}

	return -1;
}

int ltntstools_contains_pes_header(const uint8_t *buf, int lengthBytes)
{
	const char pattern[] = { 0x00, 0x00, 0x01 };
	for (int i = 0; i < lengthBytes - 3; i++) {
		if (memcmp(buf + i, pattern, sizeof(pattern)) == 0)
			return i;
	}

	return -1;
}

unsigned int ltntstools_get_section_tableid(const uint8_t *pkt)
{
	int section_offset = 5;
	if (ltntstools_has_adaption(pkt)) {
		section_offset++;
		section_offset += ltntstools_adaption_field_length(pkt);
	}

	return *(pkt + section_offset);
}

int ltntstools_is_ESPayloadType_Video(uint8_t esPayloadType)
{
    switch (esPayloadType)
    {
        case 0x01: // ISO/IEC 11172 Video
        case 0x02: // ISO/IEC 13818-2 Video
        case 0x1B: // H.264 Video
        case 0x21: // JPEG 2000"
        case 0x24:
        case 0x25:
        case 0x27:
        case 0x28:
        case 0x29:
        case 0x2A: // HEVC Video
        case 0xdb: // H.264 Video (HLS TS Encryption)
			return 1;
        default:
			return 0;
    }
}

int ltntstools_is_ESPayloadType_Audio(uint8_t esPayloadType)
{
	switch (esPayloadType)
	{
		case 0x03: // ISO/IEC 11172 Audio
		case 0x04: // ISO/IEC 13818-3 Audio
		case 0x07: // ISO/IEC 13522 MHEG
		case 0x0F: // ISO/IEC 13818-7 Audio with ADTS transport syntax
		case 0x81: // ATSC AC-3 Audio
		case 0xC1: // ATSC AC-3 Audio (HLS TS Encryption)
		case 0xC2: // ATSC EAC-3 Audio (HLS TS Encryption)
		case 0xCF: // ISO/IEC 13818-7 Audio with ADTS transport syntax (HLS TS Encryption)
			return 1;
		default:
			return 0;
	}
}

const char *ltntstools_GetESPayloadTypeDescription(uint8_t esPayloadType)
{
    switch (esPayloadType)
    {
        case 0x00:
            return "Reserved";
        case 0x01:
            return "ISO/IEC 11172 Video";
        case 0x02:
            return "ISO/IEC 13818-2 Video";
        case 0x03:
            return "ISO/IEC 11172 Audio";
        case 0x04:
            return "ISO/IEC 13818-3 Audio";
        case 0x05:
            return "ISO/IEC 13818-1 Private Section";
        case 0x06:
            return "ISO/IEC 13818-1 Private PES data packets";
        case 0x07:
            return "ISO/IEC 13522 MHEG";
        case 0x08:
            return "ISO/IEC 13818-1 Annex A DSM CC";
        case 0x09:
            return "H222.1";
        case 0x0A:
            return "ISO/IEC 13818-6 type A";
        case 0x0B:
            return "ISO/IEC 13818-6 type B";
        case 0x0C:
            return "ISO/IEC 13818-6 type C";
        case 0x0D:
            return "ISO/IEC 13818-6 type D";
        case 0x0E:
            return "ISO/IEC 13818-1 auxillary";
        case 0x0F:
            return "ISO/IEC 13818-7 Audio with ADTS transport syntax";
        case 0x10:
            return "ISO/IEC 14496-2 (MPEG-4) Visual";
        case 0x11:
            return "ISO/IEC 14496-3 Audio with the LATM transport syntax as defined in ISO/IEC 14496-3 / AMD 1";
        case 0x12:
            return "ISO/IEC 14496-1 SL-packetized stream or FlexMux stream carried in PES packets";
        case 0x13:
            return "ISO/IEC 14496-1 SL-packetized stream or FlexMux stream carried in ISO/IEC14496_sections";
        case 0x14:
            return "ISO/IEC 13818-6 Synchronized Download Protocol";
        case 0x1B:
            return "H.264 Video";
        case 0x21:
            return "JPEG 2000";
        case 0x24:
        case 0x25:
        case 0x27:
        case 0x28:
        case 0x29:
        case 0x2A:
            return "HEVC Video";
        case 0x81:
            return "ATSC AC-3 Audio";
        case 0xC1:
            /* See https://developer.apple.com/library/content/documentation/AudioVideo/Conceptual/HLS_Sample_Encryption/TransportStreamSignaling/TransportStreamSignaling.html#//apple_ref/doc/uid/TP40012862-CH3-SW1 */
            return "ATSC AC-3 Audio (HLS TS Encryption)";
        case 0xC2:
            /* See https://developer.apple.com/library/content/documentation/AudioVideo/Conceptual/HLS_Sample_Encryption/TransportStreamSignaling/TransportStreamSignaling.html#//apple_ref/doc/uid/TP40012862-CH3-SW1 */
            return "ATSC EAC-3 Audio (HLS TS Encryption)";
        case 0xCF:
            /* See https://developer.apple.com/library/content/documentation/AudioVideo/Conceptual/HLS_Sample_Encryption/TransportStreamSignaling/TransportStreamSignaling.html#//apple_ref/doc/uid/TP40012862-CH3-SW1 */
            return "ISO/IEC 13818-7 Audio with ADTS transport syntax (HLS TS Encryption)";
        case 0xdb:
            /* See https://developer.apple.com/library/content/documentation/AudioVideo/Conceptual/HLS_Sample_Encryption/TransportStreamSignaling/TransportStreamSignaling.html#//apple_ref/doc/uid/TP40012862-CH3-SW1 */
            return "H.264 Video (HLS TS Encryption)";
        default:
            if (esPayloadType < 0x80)
                return "ISO/IEC 13818-1 reserved";
            else
                return "User Private";
    }
}

void ltntstools_generateNullPacket(uint8_t *pkt)
{
        memset(pkt, 0xff, 188);
        *(pkt + 0) = 0x47;
        *(pkt + 1) = 0x1f;
        *(pkt + 2) = 0xff;
        *(pkt + 3) = 0x10;
}

static uint8_t verifyPacket[188] = { 0 };

int ltntstools_verifyPacketWith64bCounter(unsigned char *pkt, int lengthBytes, uint16_t pid, uint64_t lastCounter, uint64_t *currentCounter)
{
	if (lengthBytes < 188 || !pkt || !currentCounter)
		return -1;

	if (verifyPacket[0] == 0) {
        	memset(verifyPacket, 0xff, 188);
	}

	if (*(pkt +  0) != 0x47)
		return -1;
	if (*(pkt +  1) != (pid & 0x1fff) >> 8)
		return -1;
	if (*(pkt +  2) != pid)
		return -1;
	if ((*(pkt +  3) & 0xf0) != 0x10)
		return -1;

	/* Igonring the CC for now */

        *currentCounter  = (uint64_t)*(pkt +  8) << 56LL;
        *currentCounter |= (uint64_t)*(pkt +  9) << 48LL;
        *currentCounter |= (uint64_t)*(pkt + 10) << 40LL;
        *currentCounter |= (uint64_t)*(pkt + 11) << 32LL;
        *currentCounter |= (uint64_t)*(pkt + 12) << 24LL;
        *currentCounter |= (uint64_t)*(pkt + 13) << 16LL;
        *currentCounter |= (uint64_t)*(pkt + 14) <<  8LL;
        *currentCounter |= (uint64_t)*(pkt + 15);

	if (lastCounter + 1 != *currentCounter)
		return -1;

	if (memcmp(&verifyPacket[16], pkt + 16, 188 - 16) != 0)
		return -1;

	return 0;
}

int ltntstools_generatePacketWith64bCounter(unsigned char *pkt, int lengthBytes, uint16_t pid, uint8_t *cc, uint64_t counter)
{
	if (lengthBytes < 188 || !pkt || !cc)
		return -1;

        memset(pkt, 0xff, 188);

        *(pkt +  0) = 0x47;
        *(pkt +  1) = (pid & 0x1fff) >> 8;
        *(pkt +  2) = pid;
        *(pkt +  3) = 0x10 | ((*cc)++ & 0x0f);
        *(pkt +  8) = counter >> 56LL;
        *(pkt +  9) = counter >> 48LL;
        *(pkt + 10) = counter >> 40LL;
        *(pkt + 11) = counter >> 32LL;
        *(pkt + 12) = counter >> 24LL;
        *(pkt + 13) = counter >> 16LL;
        *(pkt + 14) = counter >>  8LL;
        *(pkt + 15) = counter;

	return 0;
}

int ltntstools_findSyncPosition(const uint8_t *buf, int lengthBytes)
{
	if (lengthBytes < 3 * 188)
		return -1;

	for (int i = 0; i < 188; i++) {
		if (buf[i] == 0x47 && buf[i + (1 * 188)] == 0x47 && buf[i + (2 * 188)] == 0x47) {
			return i;
		}
	}

	return -1;
}

int ltntstools_queryPCRs(const uint8_t *buf, int lengthBytes, uint64_t addr, struct ltntstools_pcr_position_s **array, int *arrayLength)
{
	/* Find the SYNC byte offset in a buffer of potential transport packets. */
	int offset = ltntstools_findSyncPosition(buf, lengthBytes);
	if (offset < 0)
		return -1;

	struct ltntstools_pcr_position_s *arr = NULL;
	int arrLength = 0;
	uint64_t scr;

	for (uint64_t i = offset; i < lengthBytes - offset; i += 188) {
		const uint8_t *pkt = buf + i;

		if (pkt[0] == 0x80 && pkt[12] == 0x47) {
			/* Found a RTP header, skip it */
			i += 12;
			continue;
		}

		if (ltntstools_scr((uint8_t *)pkt, &scr) < 0)
			continue;

		arr = realloc(arr, ++arrLength * sizeof(struct ltntstools_pcr_position_s));
		if (!arr)
			return -1;

		(arr + (arrLength - 1))->pid = ltntstools_pid(pkt);
		(arr + (arrLength - 1))->offset = addr + i;
		(arr + (arrLength - 1))->pcr = scr;
	}

	*array = arr;
	*arrayLength = arrLength;

	return 0; /* Success */
}

int ltntstools_pcr_position_append(struct ltntstools_pcr_position_s **array, int *arrayLength, struct ltntstools_pcr_position_s *p)
{
	*array = realloc(*array, ++(*arrayLength) * sizeof(struct ltntstools_pcr_position_s));
	if (!*array)
		return -1;

	memcpy(*array + (*arrayLength - 1), p, sizeof(*p));

	return 0; /* Success */
}
int ltntstools_queryPCR_pid(const uint8_t *buf, int lengthBytes, struct ltntstools_pcr_position_s *pos, uint16_t pcrPID, int pktAligned)
{
	int offset = 0;

	if (!pktAligned) {
		/* Find the SYNC byte offset in a buffer of potential transport packets. */
		int offset = ltntstools_findSyncPosition(buf, lengthBytes);
		if (offset < 0)
			return -1;
	}

	uint64_t scr;

	int ret = -1;
	for (uint64_t i = offset; i < lengthBytes - offset; i += 188) {
		const uint8_t *pkt = buf + i;

		if (pkt[0] == 0x80 && pkt[12] == 0x47) {
			/* Found a RTP header, skip it */
			i += 12;
			continue;
		}

		if (ltntstools_pid(pkt) != pcrPID)
			continue;

		if (ltntstools_scr((uint8_t *)pkt, &scr) < 0)
			continue;

		pos->pid = pcrPID;
		pos->offset = i;
		pos->pcr = scr;

		ret = 0; /* Success */
		break; /* We only need the first PCR */
	}

	return ret;
}

int ltntstools_file_estimate_bitrate(const char *filename, uint32_t *bps)
{
	if (!filename || !bps)
		return -1;

	/* Figure out the PCR Pid */
	struct ltntstools_pat_s *pat;
	if (ltntstools_streammodel_alloc_from_url(filename, &pat) < 0) {
		fprintf(stderr, "%s() Unable to query stream model for file\n", __func__);
		return -1;
	}

	int e = 0;
	struct ltntstools_pmt_s *pmt;
	if (ltntstools_pat_enum_services_video(pat, &e, &pmt) < 0) {
		fprintf(stderr, "%s() Unable to detect PCR PID from file.\n", __func__);
		return -1;
	}

	FILE *fh = fopen(filename, "rb");
	if (!fh) {
		ltntstools_pat_free(pat);
		return -1;
	}

	int rlen = 32 * 1048576;
	uint8_t *buf = malloc(rlen);
	if (!buf) {
		ltntstools_pat_free(pat);
		return -1;
	}

	int l = fread(buf, 1, rlen, fh);
	if (l > 0) {
		int arrayLength;
		struct ltntstools_pcr_position_s *array;
		if (ltntstools_queryPCRs(buf, l, 0, &array, &arrayLength) < 0) {
			fclose(fh);
			free(buf);
			ltntstools_pat_free(pat);
			return -1;
		}

		struct ltntstools_pcr_position_s first = { 0 }, next = { 0 };
		first.pid = 0;

		for (int i = 0; i < arrayLength; i++) {
			struct ltntstools_pcr_position_s *p = &array[i];
			if (p->pid != pmt->PCR_PID)
				continue;

			if (first.pid == 0)
				first = *p;

			next = *p;
		}

#if 0
		printf("first   offset %12" PRIu64 "  scr %14" PRIu64 "\n", first.offset, first.pcr);
		printf(" next   offset %12" PRIu64 "  scr %14" PRIu64 "\n", next.offset, next.pcr);
#endif
		uint64_t bits = (next.offset - first.offset) * 8;
		uint64_t ticks_ms = (next.pcr - first.pcr) / 27000;
		*bps = (bits / ticks_ms) * 1000;
#if 0
		printf("  time %14" PRIu64 " (ms)\n", ticks_ms);
		printf("  bits %14" PRIu64 "\n", bits);
		printf("   bps %14d\n", *bps);
#endif
	}

	fclose(fh);
	free(buf);
	ltntstools_pat_free(pat);

	return 0; /* Success */
}

void ltntstools_pts_to_ascii(char **buf, int64_t pts)
{
	if (*buf == NULL)
			*buf = malloc(16);

	/* Normalize to seconds */
	int64_t t = pts / 90000;

	int ms   = (pts / 90) % 1000;
	int secs = t % 60;
	int mins = (t / 60) % 60;
	int hrs  = (t / 3600) % 24;
	int days = t / 86400;

	sprintf(*buf, "%d.%02d:%02d:%02d.%03d",
			days, hrs, mins, secs, ms);
}

void ltntstools_pcr_to_ascii(char **buf, int64_t pcr)
{
    ltntstools_pts_to_ascii(buf, pcr / 300);
}
