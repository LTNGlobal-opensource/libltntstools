#include "libltntstools/ts.h"
#include <inttypes.h>

/* Convert a pcr base + extension field back into a 27MHz System Clock reference value.
 * Ptr points to a 6 byte raw data, which we'll parse.
 * The Adaption spec calls for the PCR to have the following format:
 * program_clock_reference_base       33bits
 * reserved                            6bits
 * program_clock_reference_extension   9bits
 */
uint64_t ltntstools_pcrToScr(unsigned char *ptr, int len)
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

int ltntstools_scr(uint8_t *pkt, uint64_t *scr)
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

int ltntstools_contains_pes_header(uint8_t *buf, int lengthBytes)
{
	const char pattern[] = { 0x00, 0x00, 0x01 };
	for (int i = 0; i < lengthBytes - 4; i++) {
		if (memcmp(buf + i, pattern, sizeof(pattern)) == 0)
			return i;
	}

	return -1;
}

unsigned int ltntstools_get_section_tableid(unsigned char *pkt)
{
	int section_offset = 5;
	if (ltntstools_has_adaption(pkt)) {
		section_offset++;
		section_offset += ltntstools_adaption_field_length(pkt);
	}

	return *(pkt + section_offset);
}

const char *ltntstools_GetESPayloadTypeDescription(unsigned char esPayloadType)
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

void ltntstools_generateNullPacket(unsigned char *pkt)
{
        memset(pkt, 0xff, 188);
        *(pkt + 0) = 0x47;
        *(pkt + 1) = 0x1f;
        *(pkt + 2) = 0xff;
        *(pkt + 3) = 0x10;
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
