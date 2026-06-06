
#include <libltntstools/nal_bitreader.h>

void NALBitReader_init(NALBitReader *br, const unsigned char *data, int size)
{
	br->data = data;
	br->size = size;
	br->bit_pos = 0;
}

void NALBitReader_skip_bits(NALBitReader *br, int n)
{
	for (int i = 0; i < n; i++) {
		NALBitReader_read_bit(br);
	}
}

void NALBitReader_skip_to_byte_aligned(NALBitReader *br)
{
	int bits_to_skip = br->bit_pos % 8;

	if (bits_to_skip) {
		NALBitReader_skip_bits(br, 8 - bits_to_skip);
	}
}

int NALBitReader_read_bit(NALBitReader *br)
{
	if (br->bit_pos >= br->size * 8) {
		return -1;
	}

	int byte_offset = br->bit_pos / 8;
	int bit_offset = 7 - (br->bit_pos % 8);
	br->bit_pos++;

	return (br->data[byte_offset] >> bit_offset) & 0x01;
}

unsigned int NALBitReader_read_bits(NALBitReader *br, int n)
{
	unsigned int result = 0;

	for (int i = 0; i < n; i++) {
		int bit = NALBitReader_read_bit(br);
		if (bit < 0) {
			return 0xFFFFFFFF;
		}
		result = (result << 1) | bit;
	}

	return result;
}

// Unsigned Exp-Golomb code
int NALBitReader_read_ue(NALBitReader *br)
{
	int leadingZeroBits = -1;

	for (int b = 0; !b; leadingZeroBits++) {
		b = NALBitReader_read_bit(br);
		if (b < 0) {
			return -1;
		}
	}
	if (leadingZeroBits > 31) {
		return -1;
	}

	int infoBits = NALBitReader_read_bits(br, leadingZeroBits);

	return (1 << leadingZeroBits) - 1 + infoBits;
}

int NALBitReader_read_se(NALBitReader *br)
{
	int codeNum = NALBitReader_read_ue(br);
	int val = (codeNum & 1) ? (int)((codeNum + 1) >> 1) : -(int)(codeNum >> 1);

    return val;
}

/* AVC/H.264 mapped Exp-Golomb for coded_block_pattern, me(v).
 *
 * intra = 1 for Intra_4x4 / Intra_8x8 / Intra_16x16 macroblocks
 * intra = 0 for inter macroblocks
 *
 * This table form is for ChromaArrayType != 0, i.e. common 4:2:0 / 4:2:2 / 4:4:4 cases.
 */
int NALBitReader_read_me(NALBitReader *br, int intra)
{
	static const int cbp_inter[48] = {
		 0, 16,  1,  2,  4,  8, 32,  3,
		 5, 10, 12, 15, 47,  7, 11, 13,
		14,  6,  9, 31, 35, 37, 42, 44,
		33, 34, 36, 40, 39, 43, 45, 46,
		17, 18, 20, 24, 19, 21, 26, 28,
		23, 27, 29, 30, 22, 25, 38, 41
	};

	static const int cbp_intra[48] = {
		47, 31, 15,  0, 23, 27, 29, 30,
		 7, 11, 13, 14, 39, 43, 45, 46,
		16,  3,  5, 10, 12, 19, 21, 26,
		28, 35, 37, 42, 44,  1,  2,  4,
		 8, 17, 18, 20, 24,  6,  9, 22,
		25, 32, 33, 34, 36, 40, 38, 41
	};

	int codeNum = NALBitReader_read_ue(br);

	if (codeNum < 0 || codeNum >= 48)
		return -1;

	return intra ? cbp_intra[codeNum] : cbp_inter[codeNum];
}

int NALBitReader_read_te(NALBitReader *br, int maxVal)
{
	if (maxVal < 1)
		return 0;

	if (maxVal == 1) {
		int bit = NALBitReader_read_bit(br);
		if (bit < 0)
			return -1;

		return !bit;
	}

	return NALBitReader_read_ue(br);
}