
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
