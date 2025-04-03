#include <stdio.h>

#include "libltntstools/ac3.h"

#include "klbitstream_readwriter.h"

void ltntstools_ac3_header_dprintf(int fd, struct ltn_ac3_header_syncframe_s *sf)
{
	dprintf(fd, "sf->syncinfo.syncword   = 0x%04x [%s]\n",
		sf->syncinfo.syncword,
		sf->syncinfo.syncword == 0x0b77 ? "correct" : "incorrect");

	dprintf(fd, "sf->syncinfo.crc1       = 0x%04x [ignored]\n", sf->syncinfo.crc1);

	dprintf(fd, "sf->syncinfo.fscod      = %6d [%s]\n",
		sf->syncinfo.fscod,
		sf->syncinfo.fscod == 0 ? "48 KHz" :
		sf->syncinfo.fscod == 1 ? "44.1 KHz" :
		sf->syncinfo.fscod == 2 ? "32 KHz" : "reserved");

	dprintf(fd, "sf->syncinfo.frmsizecod = 0x%04x [not decoded]\n", sf->syncinfo.frmsizecod);

	dprintf(fd, "sf->bsi.bsid            = %6d [%s]\n",
		sf->bsi.bsid,
		sf->bsi.bsid == 6 ? "alternative syntax (extended BSI)" :
		sf->bsi.bsid == 8 ? "traditional syntax" : "undefined");

	dprintf(fd, "sf->bsi.bsmod           = %6d [%s]\n",
		sf->bsi.bsmod,
		sf->bsi.bsmod == 0 ? "main audio service: complete main (CM)" :
		sf->bsi.bsmod == 1 ? "main audio service: music and effects (ME)" :
		sf->bsi.bsmod == 2 ? "associated service: visually impaired (VI)" :
		sf->bsi.bsmod == 3 ? "associated service: hearing impaired (HI)" :
		sf->bsi.bsmod == 4 ? "associated service: dialogue (D)" :
		sf->bsi.bsmod == 5 ? "associated service: commentary (C)" :
		sf->bsi.bsmod == 6 ? "associated service: emergency (E)" : "not decoded");

	dprintf(fd, "sf->bsi.acmod           = %6d [%s], nfchans = %d\n",
		sf->bsi.acmod,
		sf->bsi.acmod == 0 ? "1+1 - Ch1, Ch2" :
		sf->bsi.acmod == 1 ? "1/0 - C" :
		sf->bsi.acmod == 2 ? "2/0 - L, R" :
		sf->bsi.acmod == 3 ? "3/0 - L, C, R" :
		sf->bsi.acmod == 4 ? "2/1 - L, R, S" :
		sf->bsi.acmod == 5 ? "3/1 - L, C, R, S" :
		sf->bsi.acmod == 6 ? "2/2 - L, R, SL, SR" : "3/2 - L, C, R, SL, SR",
		sf->bsi.acmod_nfchans);

	if ((sf->bsi.acmod & 0x1) && (sf->bsi.acmod != 0x1)) {
		/* If 3 front channels */
		dprintf(fd, "sf->bsi.cmixlev         = %6d [%s]\n",
			sf->bsi.cmixlev,
			sf->bsi.cmixlev == 0 ? "-3.0 dB" :
			sf->bsi.cmixlev == 1 ? "-4.5 dB" :
			sf->bsi.cmixlev == 2 ? "-6.0 dB" : "reserved");
	}
	if (sf->bsi.acmod & 0x4) {
		/* If a surround sound exists */
		dprintf(fd, "sf->bsi.surmixlev       = %6d [%s]\n",
			sf->bsi.surmixlev,
			sf->bsi.surmixlev == 0 ? "-3.0 dB" :
			sf->bsi.surmixlev == 1 ? "-6.0 dB" :
			sf->bsi.surmixlev == 2 ? "0" : "reserved");
	}
	if (sf->bsi.acmod == 0x2) {
		/* If in 2/0 mode */
		dprintf(fd, "sf->bsi.dsurmod         = %6d [%s]\n",
			sf->bsi.dsurmod,
			sf->bsi.dsurmod == 0 ? "not indicated" :
			sf->bsi.dsurmod == 1 ? "Not Dolby Surround encoded" :
			sf->bsi.dsurmod == 2 ? "Dolby Surround encoded" : "reserved");
	}

	dprintf(fd, "sf->bsi.lfeon           = %6d [%s]\n",
		sf->bsi.lfeon,
		sf->bsi.lfeon ? "On" : "Off");

	dprintf(fd, "sf->bsi.dialnorm        = %6d\n", sf->bsi.dialnorm);
	dprintf(fd, "sf->bsi.compre          = %6d [compression_control %s]\n",
		sf->bsi.compre,
		sf->bsi.compre ? "enabled" : "disabled");
	if (sf->bsi.compre) {
		dprintf(fd, "sf->bsi.compr           = %6d [compression_gain]\n", sf->bsi.compr);
	}
	dprintf(fd, "sf->bsi.langcode        = %6d [%s]\n",
		sf->bsi.langcode,
		sf->bsi.langcode ? "Present" : "Not Present");

	if (sf->bsi.langcode) {
		dprintf(fd, "sf->bsi.langcod      = %6d [not decoded]\n", sf->bsi.langcod);
	}

	dprintf(fd, "sf->bsi.audprodie       = %6d [%s]\n",
		sf->bsi.audprodie,
		sf->bsi.audprodie ? "Mixing room details present" : "Mixing room details not present");
	if (sf->bsi.audprodie) {
		dprintf(fd, "sf->bsi.mixlevel      = %6d [not decoded]\n", sf->bsi.mixlevel);
		dprintf(fd, "sf->bsi.roomtyp       = %6d [not decoded]\n", sf->bsi.roomtyp);
	}

	if (sf->bsi.acmod == 0) {
		/* if 1+1 */
		dprintf(fd, "sf->bsi.dialnorm2     = %6d\n", sf->bsi.dialnorm2);
		dprintf(fd, "sf->bsi.compr2e       = %6d\n", sf->bsi.compr2e);
		if (sf->bsi.compr2e) {
			dprintf(fd, "sf->bsi.compr2        = %6d\n", sf->bsi.compr2);
		}
		dprintf(fd, "sf->bsi.langcod2e       = %6d\n", sf->bsi.langcod2e);
		if (sf->bsi.langcod2e) {
			dprintf(fd, "sf->bsi.langcod2     = %6d [not decoded]\n", sf->bsi.langcod2);
		}
		dprintf(fd, "sf->bsi.audprodi2e       = %6d\n", sf->bsi.audprodi2e);
		if (sf->bsi.audprodi2e) {
			dprintf(fd, "sf->bsi.mixlevel2     = %6d [not decoded]\n", sf->bsi.mixlevel2);
			dprintf(fd, "sf->bsi.roomtyp2      = %6d [not decoded]\n", sf->bsi.roomtyp2);
		}
	}

	dprintf(fd, "sf->bsi.copyrightb      = %6d [%s]\n",
		sf->bsi.copyrightb,
		sf->bsi.copyrightb == 1 ? "Yes" : "No");
	dprintf(fd, "sf->bsi.origbs          = %6d [%s]\n",
		sf->bsi.origbs,
		sf->bsi.origbs == 1 ? "Yes" : "No");

	if (sf->bsi.bsid == 8) {
		dprintf(fd, "sf->bsi.timecod1e       = %6d\n", sf->bsi.timecod1e);
		if (sf->bsi.timecod1e) {
			dprintf(fd, "sf->bsi.timecod1       = %6d\n", sf->bsi.timecod1);		
		}
		dprintf(fd, "sf->bsi.timecod2e       = %6d\n", sf->bsi.timecod2e);
		if (sf->bsi.timecod2e) {
			dprintf(fd, "sf->bsi.timecod2       = %6d\n", sf->bsi.timecod2);		
		}
	} else
	if (sf->bsi.bsid == 6) {

		dprintf(fd, "sf->bsi.xbsi1e          = %6d [%s]\n",
			sf->bsi.xbsi1e,
			sf->bsi.xbsi1e ? "Yes" : "No");

		if (sf->bsi.xbsi1e) {
			dprintf(fd, "sf->bsi.dmixmod         = %6d\n", sf->bsi.dmixmod);
			dprintf(fd, "sf->bsi.ltrtcmixlev     = %6d\n", sf->bsi.ltrtcmixlev);
			dprintf(fd, "sf->bsi.ltrtsurmixlev   = %6d\n", sf->bsi.ltrtsurmixlev);
			dprintf(fd, "sf->bsi.lorocmixlev     = %6d\n", sf->bsi.lorocmixlev);
			dprintf(fd, "sf->bsi.lorosurmixlev   = %6d\n", sf->bsi.lorosurmixlev);
		}

		dprintf(fd, "sf->bsi.xbsi2e          = %6d [%s]\n",
			sf->bsi.xbsi2e,
			sf->bsi.xbsi2e ? "Yes" : "No");

		if (sf->bsi.xbsi2e) {
			dprintf(fd, "sf->bsi.dsurexmod       = %6d\n", sf->bsi.dsurexmod);
			dprintf(fd, "sf->bsi.dheadphonmod    = %6d\n", sf->bsi.dheadphonmod );
			dprintf(fd, "sf->bsi.adconvtyp       = %6d\n", sf->bsi.adconvtyp );
			dprintf(fd, "sf->bsi.xbsi2           = %6d\n", sf->bsi.xbsi2 );
			dprintf(fd, "sf->bsi.encinfo         = %6d\n", sf->bsi.encinfo );
		}
	}	
	
	dprintf(fd, "sf->bsi.addbsil         = %6d [reserved padding bytes]\n", sf->bsi.addbsil);
	if (sf->bsi.addbsil) {
		dprintf(fd, "sf->bsi.addbsi[] = ");
		for (int i = 0; i < (sf->bsi.addbsil + 1); i++) {
			dprintf(fd, "%02x ", sf->bsi.addbsi[i]);
		}
		dprintf(fd, "\n");
	}

	for (int i = 0; i < 6; i++) {
		struct ltn_ac3_header_audioblk_s *b = &sf->audioblk[i];

		for (int ch = 0; ch < sf->bsi.acmod_nfchans; ch++) {
			dprintf(fd, "sf->audblk[%d].blksw[ch%d]         = %6d\n", i, ch + 1, b->blksw[ch]);
		}
		for (int ch = 0; ch < sf->bsi.acmod_nfchans; ch++) {
			dprintf(fd, "sf->audblk[%d].dithflag[ch%d]      = %6d\n", i, ch + 1, b->dithflag[ch]);
		}
		dprintf(fd, "sf->audblk[%d].dynrnge            = %6d [%s]\n", i,
			b->dynrnge,
			b->dynrnge ? "Yes" : "No");
		if (b->dynrnge) {
			dprintf(fd, "sf->audblk[%d].dynrng           = %6d\n", i, b->dynrnge);
		}
		break; /* We support a tiny faction of audblk, don't proceed */
	}
};

int ltntstools_ac3_header_parse(struct ltn_ac3_header_syncframe_s *sf, unsigned char *buf, int lengthBytes)
{
	struct klbs_context_s pbs, *bs = &pbs;
	klbs_init(bs);
	klbs_read_set_buffer(bs, buf, lengthBytes);

	/* See A52-201212-17-2.pdf page 30 */

	/* syninfo() length is 8 bytes.
	 * bsi() length is a minimum of 5 bytes but probably a lot more to max 14 ish excluding bsil
	 */
	if (lengthBytes < (8 + 14)) {
		return -1; /* Invalid */
	}

	/* syncinfo() */
	sf->syncinfo.syncword    = klbs_read_bits(bs, 16);
	sf->syncinfo.crc1        = klbs_read_bits(bs, 16);
	sf->syncinfo.fscod       = klbs_read_bits(bs,  2);
	sf->syncinfo.frmsizecod  = klbs_read_bits(bs,  6);

	/* bsi() */
	sf->bsi.bsid        = klbs_read_bits(bs, 5);
	sf->bsi.bsmod       = klbs_read_bits(bs, 3);
	sf->bsi.acmod       = klbs_read_bits(bs, 3);
	sf->bsi.cmixlev     = 0;
	sf->bsi.surmixlev   = 0;
	sf->bsi.dsurmod     = 0;

	uint32_t nfchans[8] = { 2, 1, 2, 3, 3, 4, 4, 5 }; /* See Table 5.8 */
	sf->bsi.acmod_nfchans = nfchans[sf->bsi.acmod];

	if ((sf->bsi.acmod & 0x1) && (sf->bsi.acmod != 0x1)) {
		/* If 3 front channels */
		sf->bsi.cmixlev = klbs_read_bits(bs, 2);
	}
	if (sf->bsi.acmod & 0x4) {
		/* If a surround sound exists */
		sf->bsi.surmixlev = klbs_read_bits(bs, 2);
	}
	if (sf->bsi.acmod == 0x2) {
		/* If in 2/0 mode */
		sf->bsi.dsurmod = klbs_read_bits(bs, 2);
	}

	sf->bsi.lfeon       = klbs_read_bits(bs, 1);
	sf->bsi.dialnorm    = klbs_read_bits(bs, 5);
	sf->bsi.dialnorm   *= -1;
	sf->bsi.compre      = klbs_read_bits(bs, 1);
	sf->bsi.compr       = 0;
	if (sf->bsi.compre) {
		sf->bsi.compr   = klbs_read_bits(bs, 8);
	}

	sf->bsi.langcode    = klbs_read_bits(bs, 1);
	if (sf->bsi.langcode) {
		sf->bsi.langcod = klbs_read_bits(bs, 8);
	}

	sf->bsi.audprodie    = klbs_read_bits(bs, 1);
	if (sf->bsi.audprodie) {
		sf->bsi.mixlevel = klbs_read_bits(bs, 5);
		sf->bsi.roomtyp  = klbs_read_bits(bs, 2);
	}

	if (sf->bsi.acmod == 0) {
		/* if 1 + 1 */
		sf->bsi.dialnorm2     = klbs_read_bits(bs, 5);
		sf->bsi.dialnorm2    *= -1;

		sf->bsi.compr2e       = klbs_read_bits(bs, 1);
		if (sf->bsi.compr2e) {
			sf->bsi.compr2    = klbs_read_bits(bs, 8);
		}
		sf->bsi.langcod2e     = klbs_read_bits(bs, 1);
		if (sf->bsi.langcod2e) {
			sf->bsi.langcod2  = klbs_read_bits(bs, 8);
		}
		sf->bsi.audprodi2e    = klbs_read_bits(bs, 1);
		if (sf->bsi.audprodi2e) {
			sf->bsi.mixlevel2 = klbs_read_bits(bs, 5);
			sf->bsi.roomtyp2  = klbs_read_bits(bs, 2);
		}
	}

	sf->bsi.copyrightb  = klbs_read_bits(bs, 1);
	sf->bsi.origbs      = klbs_read_bits(bs, 1);

	if (sf->bsi.bsid == 8) {
		/* Older more traditional syntax for the BSI, before the Alternative syntax appeared in appendix D */
		sf->bsi.timecod1e   = klbs_read_bits(bs, 1);
		if (sf->bsi.timecod1e) {
			sf->bsi.timecod1 = klbs_read_bits(bs, 14);
		}
		sf->bsi.timecod2e   = klbs_read_bits(bs, 1);
		if (sf->bsi.timecod2e) {
			sf->bsi.timecod2 = klbs_read_bits(bs, 14);
		}
	} else 
	if (sf->bsi.bsid == 6) {
		/* See Table D2.1 Bit Stream Information (Alternate Bit Stream Syntax)
		 * An AC-3 bit stream shall have the alternate bit stream syntax described
		 * in this annex when the bit stream identification (bsid) field is set to 6.
		 */
		sf->bsi.xbsi1e      = klbs_read_bits(bs, 1);
		if (sf->bsi.xbsi1e) {
			sf->bsi.dmixmod       = klbs_read_bits(bs, 2);
			sf->bsi.ltrtcmixlev   = klbs_read_bits(bs, 3);
			sf->bsi.ltrtsurmixlev = klbs_read_bits(bs, 3);
			sf->bsi.lorocmixlev   = klbs_read_bits(bs, 3);
			sf->bsi.lorosurmixlev = klbs_read_bits(bs, 3);
		}

		sf->bsi.xbsi2e           = klbs_read_bits(bs, 1);
		if (sf->bsi.xbsi2e) {
			sf->bsi.dsurexmod    = klbs_read_bits(bs, 2);
			sf->bsi.dheadphonmod = klbs_read_bits(bs, 2);
			sf->bsi.adconvtyp    = klbs_read_bits(bs, 1);
			sf->bsi.xbsi2        = klbs_read_bits(bs, 8);
			sf->bsi.encinfo      = klbs_read_bits(bs, 1);
		}
	}

	sf->bsi.addbsil          = 0;
	sf->bsi.addbsie          = klbs_read_bits(bs, 1);
	if (sf->bsi.addbsie) {
		sf->bsi.addbsil      = klbs_read_bits(bs, 6);
		/* Additional bitstream information length */
		/* 5.4.2.30. This 6-bit code, which exists only if addbsie is a ‘1’, indicates the length in
		 * bytes of additional bitstream information. The valid range of addbsil is 0–63,
		 * indicating 1–64 additional bytes, respectively. The decoder is not required to
		 * interpret this information, and thus shall skip over this number of bytes following
		 * in the data stream.
		 */
		for (int i = 0; i < (sf->bsi.addbsil + 1); i++) {
			sf->bsi.addbsi[i] = klbs_read_bits(bs, 8);
		}
	}

	/* parse the audio blocks */
	for (int i = 0; i < 6; i++) {
		struct ltn_ac3_header_audioblk_s *b = &sf->audioblk[i];

		for (int ch = 0; ch < sf->bsi.acmod_nfchans; ch++) {
			b->blksw[ch] = klbs_read_bits(bs, 1);
		}
		for (int ch = 0; ch < sf->bsi.acmod_nfchans; ch++) {
			b->dithflag[ch] = klbs_read_bits(bs, 1);
		}

		b->dynrnge = klbs_read_bits(bs, 1);
		if (b->dynrnge) {
			b->dynrng = klbs_read_bits(bs, 8);
		}
	}
	return 0; /* Success */
}
