#ifndef _LIBLTNTSTOOLS_AC3_H
#define _LIBLTNTSTOOLS_AC3_H

/**
 * @file        segmentwriter.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       A threaded file writer. Produces single or segmented recordings.
 *              Capable of supporting any kind of bytestream, targeted at MPEG-TS streams.
 */
#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEGMENTEDWRITER_SINGLE_FILE 0
#define SEGMENTEDWRITER_SEGMENTED   1

struct ltn_ac3_header_syncinfo_s {
	uint32_t syncword;
	uint32_t crc1;
	uint32_t fscod;
	uint32_t frmsizecod;
};
struct ltn_ac3_header_bsi_s {
	uint32_t bsid;
	uint32_t bsmod;
	uint32_t acmod;
	uint32_t acmod_nfchans; /* Value derived from acmod */
	uint32_t cmixlev;
	uint32_t surmixlev;
	uint32_t dsurmod;
	uint32_t lfeon;
	int32_t  dialnorm;
	uint32_t compre;
	uint32_t compr;
	uint32_t langcode;
	uint32_t langcod;
	uint32_t audprodie;
	uint32_t mixlevel;
	uint32_t roomtyp;

	/* When acmod == 0, 1+1 mode */
	int32_t  dialnorm2;
	uint32_t compr2e;
	uint32_t compr2;
	uint32_t langcod2e;
	uint32_t langcod2;
	uint32_t audprodi2e;
	uint32_t mixlevel2;
	uint32_t roomtyp2;

	uint32_t copyrightb;
	uint32_t origbs;
	uint32_t timecod1e;
	uint32_t timecod1;
	uint32_t timecod2e;
	uint32_t timecod2;

	uint32_t addbsie;
	uint32_t addbsil;
	uint8_t  addbsi[64];

	/* See Table D2.1 bsi() syntax when bsid field set to 6 */
	uint32_t xbsi1e;
	/* If xbsi1e */
	uint32_t dmixmod;
	uint32_t ltrtcmixlev;
	uint32_t ltrtsurmixlev;
	uint32_t lorocmixlev;
	uint32_t lorosurmixlev;

	uint32_t xbsi2e;
	/* If xbsi2e */
	uint32_t dsurexmod;
	uint32_t dheadphonmod;
	uint32_t adconvtyp;
	uint32_t xbsi2;
	uint32_t encinfo;

};

struct ltn_ac3_header_audioblk_s {
	uint32_t blksw[8];
	uint32_t dithflag[8];
	uint32_t dynrnge;
	uint32_t dynrng;
};

struct ltn_ac3_header_syncframe_s {
	struct ltn_ac3_header_syncinfo_s syncinfo;
	struct ltn_ac3_header_bsi_s bsi;
	struct ltn_ac3_header_audioblk_s audioblk[6];
};

/**
 * @brief       Print to a filedescriptor  the contents of struct ltn_ac3_header_syncframe_s 
 * @param[in]   int fd - filedescriptor, or STDOUT_FILENO for console
 * @param[in]   struct ltn_ac3_header_syncframe_s *ptr - object
 */
void ltntstools_ac3_header_dprintf(int fd, struct ltn_ac3_header_syncframe_s *sf);

/**
 * @brief       Parse an AC3 ES payload stream (beginning with 0x0b77 sequence) into an object
 * @param[in]   struct ltn_ac3_header_syncframe_s *ptr - object
 * @param[in]   unsigned char *buf - AC3 ES payload buffer
 * @param[in]   int lengthBytes - length of buffer in bytes
 * @return      0 on success, else < 0.
 */
int ltntstools_ac3_header_parse(struct ltn_ac3_header_syncframe_s *sf, unsigned char *buf, int lengthBytes);

#ifdef __cplusplus
};
#endif

#endif /* _LIBLTNTSTOOLS_AC3_H */


