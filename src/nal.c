#include "libltntstools/nal.h"
#include <inttypes.h>

int ltn_nal_findHeader(const uint8_t *buffer, int lengthBytes, int *offset)
{
	const uint8_t sig[] = { 0, 0, 1 };

	for (int i = (*offset + 1); i < lengthBytes - sizeof(sig); i++) {
		if (memcmp(buffer + i, sig, sizeof(sig)) == 0) {
			*offset = i;
			return 0; /* Success */
		}
	}

	return -1; /* Not found */
}

static struct hevcNal_s {
	const char *name;
	const char *type;
} hevcNals[] = {
	[ 0] = { "TRAIL_N", .type = "AUTO" },
	[ 1] = { "TRAIL_R", .type = "IDR" },
	[ 2] = {   "TSA_N", .type = "I" },
	[ 3] = {   "TSA_R", .type = "P" },
	[ 4] = {  "STSA_N", .type = "BREF" },
	[ 5] = {  "STSA_R", .type = "B" },
	[ 6] = { "RADL_N" },
	[ 7] = { "RADL_R" },
	[ 8] = { "RASL_N" },
	[ 9] = { "RASL_R" },

	[19] = { "IDR_W_RADL", .type = "IDR" },
	[20] = { "IDR_N_LP" },
	[21] = { "CRA" },

	[32] = { "VPS" },
	[33] = { "SPS" },
	[34] = { "PPS" },
	[35] = { "AUD" },
	[36] = { "EOS" },
	[37] = { "EOB" },
	[38] = { "FD" },
	[39] = { "PREFIX_SEI" },
	[40] = { "SUFFIX_SEI" },
};

const char *hevcNals_lookupName(int nalType)
{
	return hevcNals[nalType].name;
}

const char *hevcNals_lookupType(int nalType)
{
	return hevcNals[nalType].type;
}

char *ltn_nal_hevc_findNalTypes(const uint8_t *buffer, int lengthBytes)
{
	char *arr = malloc(128);
	arr[0] = 0;

	int items = 0;
	int offset = -1;
	while (ltn_nal_findHeader(buffer, lengthBytes, &offset) == 0) {
		unsigned int nalType = (buffer[offset + 3] >> 1) & 0x3f;
		const char *nalName = hevcNals_lookupName(nalType);
		const char *nalTypeDesc = hevcNals_lookupType(nalType);

		if (items++ > 0)
			sprintf(arr + strlen(arr), ", ");

		sprintf(arr + strlen(arr), "%s", nalName);
#if 0
		printf("%6d: %02x %02x %02x %02x : type %2d (%s)\n",
			offset,
			buffer[offset + 0],
			buffer[offset + 1],
			buffer[offset + 2],
			buffer[offset + 3],
			nalType,
			nalName);
#endif
	}
	
	return arr;
}

