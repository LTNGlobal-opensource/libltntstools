#ifndef NAL_BITREADER_H
#define NAL_BITREADER_H

/**
 * @file        nal_bitreader.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2025 LTN Global,Inc. All Rights Reserved.
 * @brief       Helper functions to query bitstreams found within nals.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const unsigned char *data;
    int size;
    int bit_pos;
} NALBitReader;

void NALBitReader_init(NALBitReader *br, const unsigned char *data, int size);

void NALBitReader_skip_bits(NALBitReader *br, int n);

void NALBitReader_skip_to_byte_aligned(NALBitReader *br);

int NALBitReader_read_bit(NALBitReader *br);

unsigned int NALBitReader_read_bits(NALBitReader *br, int n);

// Unsigned Exp-Golomb code
int NALBitReader_read_ue(NALBitReader *br);

int NALBitReader_read_se(NALBitReader *br);

int NALBitReader_read_me(NALBitReader *br, int intra);

int NALBitReader_read_te(NALBitReader *br, int maxVal);

#ifdef __cplusplus
};
#endif

#endif /* NAL_BITREADER.h */
