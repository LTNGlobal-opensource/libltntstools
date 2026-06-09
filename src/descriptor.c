
#include <stdio.h>
#include "libltntstools/ltntstools.h"

struct ltntstools_descriptor_tag_description_s
{
	uint8_t tag;
	const char *description;
};

static const struct ltntstools_descriptor_tag_description_s descriptor_tag_descriptions[] = {
	{ 0x00, "Reserved" },
	{ 0x01, "Forbidden" },
	{ 0x02, "Video stream descriptor" },
	{ 0x03, "Audio stream descriptor" },
	{ 0x04, "Hierarchy descriptor" },
	{ 0x05, "Registration descriptor (ATSC GA94, DTS, SMPTE registrations)" },
	{ 0x06, "Data stream alignment descriptor" },
	{ 0x07, "Target background grid descriptor" },
	{ 0x08, "Video window descriptor" },
	{ 0x09, "CA descriptor / Conditional Access descriptor" },
	{ 0x0a, "ISO 639 language descriptor (audio language)" },
	{ 0x0b, "System clock descriptor" },
	{ 0x0c, "Multiplex buffer utilization descriptor" },
	{ 0x0d, "Copyright descriptor" },
	{ 0x0e, "Maximum bitrate descriptor (max ES bitrate)" },
	{ 0x0f, "Private data indicator descriptor" },
	{ 0x10, "Smoothing buffer descriptor" },
	{ 0x11, "STD descriptor" },
	{ 0x12, "IBP descriptor" },
	{ 0x13, "Carousel identifier descriptor" },
	{ 0x14, "Association tag descriptor" },
	{ 0x15, "Deferred association tags descriptor" },
	{ 0x16, "ISO/IEC 13818-1 reserved" },
	{ 0x17, "NPT reference descriptor" },
	{ 0x18, "NPT endpoint descriptor" },
	{ 0x19, "Stream mode descriptor" },
	{ 0x1a, "Stream event descriptor" },
	{ 0x1b, "MPEG-4 video descriptor" },
	{ 0x1c, "MPEG-4 audio descriptor" },
	{ 0x1d, "IOD descriptor" },
	{ 0x1e, "SL descriptor" },
	{ 0x1f, "FMC descriptor" },
	{ 0x20, "External ES ID descriptor" },
	{ 0x21, "MuxCode descriptor" },
	{ 0x22, "FmxBufferSize descriptor" },
	{ 0x23, "MultiplexBuffer descriptor" },
	{ 0x24, "Content labeling descriptor" },
	{ 0x25, "Metadata pointer descriptor" },
	{ 0x26, "Metadata descriptor" },
	{ 0x27, "Metadata STD descriptor" },
	{ 0x28, "AVC video descriptor" },
	{ 0x29, "IPMP descriptor" },
	{ 0x2a, "AVC timing and HRD descriptor" },
	{ 0x2b, "MPEG-2 AAC audio descriptor" },
	{ 0x2c, "FlexMuxTiming descriptor" },
	{ 0x2d, "MPEG-4 text descriptor" },
	{ 0x2e, "MPEG-4 audio extension descriptor" },
	{ 0x2f, "Auxiliary video stream descriptor" },
	{ 0x30, "SVC extension descriptor" },
	{ 0x31, "MVC extension descriptor" },
	{ 0x32, "J2K video descriptor" },
	{ 0x33, "MVC operation point descriptor" },
	{ 0x34, "MPEG-2 stereoscopic video format descriptor" },
	{ 0x35, "Stereoscopic program info descriptor" },
	{ 0x36, "Stereoscopic video info descriptor" },
	{ 0x3f, "Extension descriptor" },
	{ 0x40, "Network name descriptor (DVB)" },
	{ 0x41, "Service list descriptor (DVB)" },
	{ 0x42, "Stuffing descriptor (DVB)" },
	{ 0x43, "Satellite delivery system descriptor (DVB)" },
	{ 0x44, "Cable delivery system descriptor (DVB)" },
	{ 0x45, "VBI data descriptor (DVB)" },
	{ 0x46, "VBI teletext descriptor (DVB)" },
	{ 0x47, "Bouquet name descriptor (DVB)" },
	{ 0x48, "Service descriptor (DVB)" },
	{ 0x49, "Country availability descriptor (DVB)" },
	{ 0x4a, "Linkage descriptor (DVB)" },
	{ 0x4b, "NVOD reference descriptor (DVB)" },
	{ 0x4c, "Time shifted service descriptor (DVB)" },
	{ 0x4d, "Short event descriptor (DVB)" },
	{ 0x4e, "Extended event descriptor (DVB)" },
	{ 0x4f, "Time shifted event descriptor (DVB)" },
	{ 0x50, "Component descriptor (DVB)" },
	{ 0x51, "Mosaic descriptor (DVB)" },
	{ 0x52, "Stream identifier descriptor (DVB)" },
	{ 0x53, "CA identifier descriptor (DVB)" },
	{ 0x54, "Content descriptor (DVB)" },
	{ 0x55, "Parental rating descriptor (DVB)" },
	{ 0x56, "Teletext descriptor (DVB)" },
	{ 0x57, "Telephone descriptor (DVB)" },
	{ 0x58, "Local time offset descriptor (DVB)" },
	{ 0x59, "Subtitling descriptor (DVB)" },
	{ 0x5a, "Terrestrial delivery system descriptor (DVB)" },
	{ 0x5b, "Multilingual network name descriptor (DVB)" },
	{ 0x5c, "Multilingual bouquet name descriptor (DVB)" },
	{ 0x5d, "Multilingual service name descriptor (DVB)" },
	{ 0x5e, "Multilingual component descriptor (DVB)" },
	{ 0x5f, "Private data specifier descriptor (DVB)" },
	{ 0x60, "Service move descriptor (DVB)" },
	{ 0x61, "Short smoothing buffer descriptor (DVB)" },
	{ 0x62, "Frequency list descriptor (DVB)" },
	{ 0x63, "Partial transport stream descriptor (DVB)" },
	{ 0x64, "Data broadcast descriptor (DVB)" },
	{ 0x65, "CA system descriptor (DVB)" },
	{ 0x66, "Data broadcast id descriptor (DVB)" },
	{ 0x67, "Transport stream descriptor (DVB)" },
	{ 0x68, "DSNG descriptor (DVB)" },
	{ 0x69, "PDC descriptor (DVB)" },
	{ 0x6a, "AC-3 descriptor (DVB)" },
	{ 0x6b, "Ancillary data descriptor (DVB)" },
	{ 0x6c, "Cell list descriptor (DVB)" },
	{ 0x6d, "Cell frequency link descriptor (DVB)" },
	{ 0x6e, "Announcement support descriptor (DVB)" },
	{ 0x6f, "Application signalling descriptor (DVB)" },
	{ 0x70, "Adaptation field data descriptor (DVB)" },
	{ 0x71, "Service identifier descriptor (DVB)" },
	{ 0x72, "Service availability descriptor (DVB)" },
	{ 0x73, "Default authority descriptor (DVB)" },
	{ 0x74, "Related content descriptor (DVB)" },
	{ 0x75, "TVA id descriptor (DVB)" },
	{ 0x76, "Content identifier descriptor (DVB)" },
	{ 0x77, "Time slice FEC identifier descriptor (DVB)" },
	{ 0x78, "ECM repetition rate descriptor (DVB)" },
	{ 0x79, "S2 satellite delivery system descriptor (DVB)" },
	{ 0x7a, "Enhanced AC-3 descriptor (DVB)" },
	{ 0x7b, "DTS descriptor (DVB)" },
	{ 0x7c, "AAC descriptor (DVB)" },
	{ 0x7d, "XAIT location descriptor (DVB)" },
	{ 0x7e, "FTA content management descriptor (DVB)" },
	{ 0x7f, "Extension descriptor (DVB)" },
	{ 0x80, "Stuffing descriptor (ATSC A/65)" },
	{ 0x81, "AC-3 descriptor (ATSC/private usage)" },
	{ 0x86, "Caption service descriptor (ATSC 608/708 captions)" },
	{ 0x87, "Content advisory descriptor (ATSC A/65)" },
	{ 0xa0, "Extended channel name descriptor (ATSC A/65)" },
	{ 0xa1, "Service location descriptor (ATSC A/65)" },
	{ 0xa2, "Time shifted service descriptor (ATSC A/65)" },
	{ 0xa3, "Component name descriptor (ATSC A/65)" },
	{ 0xa8, "DCC departing request descriptor (ATSC A/65)" },
	{ 0xa9, "DCC arriving request descriptor (ATSC A/65)" },
	{ 0xaa, "Redistribution control descriptor (ATSC A/65)" },
	{ 0xab, "Genre descriptor (ATSC A/65)" },
	{ 0xad, "ATSC private information descriptor" },
	{ 0xb6, "Content identifier descriptor (ATSC A/57/A65)" },
	{ 0xcc, "E-AC-3 audio stream descriptor (ATSC A/65)" },
};

const char *ltntstools_descriptor_tag_description(uint8_t tag)
{
	for (unsigned int i = 0; i < sizeof(descriptor_tag_descriptions) / sizeof(descriptor_tag_descriptions[0]); i++) {
		if (descriptor_tag_descriptions[i].tag == tag) {
			return descriptor_tag_descriptions[i].description;
		}
	}

	if (tag < 0x40)
		return "ISO/IEC 13818-1 reserved";
	if (tag >= 0xa0 && tag <= 0xaf)
		return "Vendor/private descriptor";
	return "User Private";
}

int ltntstools_descriptor_list_add(struct ltntstools_descriptor_list_s *list,
	uint8_t tag, uint8_t *src, uint8_t lengthBytes)
{
	if (list->count == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX)
		return -1;

	if (lengthBytes > 255)
		return -1;

	if (!src)
		return -1;

	if (!list)
		return -1;

	struct ltntstools_descriptor_entry_s *d = &list->array[list->count];

	d->tag = tag;
	d->len = lengthBytes;
	memcpy(&d->data[0], src, d->len);
	
	list->count++;
	return 0;
}

int ltntstools_descriptor_list_contains_smpte2064_registration(struct ltntstools_descriptor_list_s *list)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		if (d->tag == 0x05 && d->len == 0x04) {
			if (d->data[0] == 'L' && d->data[1] == 'I' && d->data[2] == 'P' && d->data[3] == 'S') {
				found = 1;
				break;
			}
		}
	}

	return found;
}

int ltntstools_descriptor_list_contains_scte35_cue_registration(struct ltntstools_descriptor_list_s *list)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		if (d->tag == 0x05 && d->len == 0x04) {
			if (d->data[0] == 'C' && d->data[1] == 'U' && d->data[2] == 'E' && d->data[3] == 'I') {
				found = 1;
				break;
			}
		}
	}

	return found;
}

int ltntstools_descriptor_list_contains_video_av1_registration(struct ltntstools_descriptor_list_s *list)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		if (d->tag == 0x05 && d->len >= 0x04) {
			if (d->data[0] == 'A' && d->data[1] == 'V' && d->data[2] == '0' && d->data[3] == '1') {
				found = 1;
				break;
			}
		}
	}

	return found;
}

int ltntstools_descriptor_list_contains_teletext(struct ltntstools_descriptor_list_s *list)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		/* EN300468 - teletext_descriptor */
		if (d->tag == 0x56 && d->len == 0x05) {
			found = 1;
			break;
		}
	}

	return found;
}

int ltntstools_descriptor_list_contains_smpte2038_registration(struct ltntstools_descriptor_list_s *list)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		if (d->tag == 0xc4 && d->len == 0x09) {
			char *s = "SMPTE2038"; /* LTN Encoder */
			if (memcmp(d->data, s, strlen(s)) == 0) {
				found = 1;
				break;
			}
		} else 
		if (d->tag == 0x05 && d->len == 0x04) {
			char *t = "VANC"; /* SMPTE2038:2008 specification */
			if (memcmp(d->data, t, strlen(t)) == 0) {
				found = 1;
				break;
			}
		} else 
		if (d->tag == 0xc4 && d->len == 0) {
			/* "This structure may be used to convey additional information about the ANC data component.
			 *  The use is optional and currently undefined. Compliant receive devices shall ignore
			 *  unrecognized descriptors."
			 */
			found = 1;
			break;
		}
	}

	return found;
}

int ltntstools_descriptor_list_contains_ltn_encoder_sw_version(struct ltntstools_descriptor_list_s *list,
	unsigned int *major, unsigned int *minor, unsigned int *patch)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		/* Look for tag a2 length 4 and embedded tag 1 (sw version) */

		if (d->tag == 0xa2 && d->len == 0x04 && d->data[0] == 0x01) {
			*major = d->data[1];
			*minor = d->data[2];
			*patch = d->data[3];
			found = 1;
			break;
		}
	}

	return found;
}

int ltntstools_descriptor_list_contains_iso639_audio_descriptor(struct ltntstools_descriptor_list_s *list,
	unsigned char *lang, unsigned int *type)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		/* Find the first iso639_lang_descriptor */
		if (d->tag == 0x0a && d->len == 0x04) {
			*(lang + 0) = d->data[0];
			*(lang + 1) = d->data[1];
			*(lang + 2) = d->data[2];
			*type = d->data[3];
			found = 1;
			break;
		}
	}

	return found;
}
