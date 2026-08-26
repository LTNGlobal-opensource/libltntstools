/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Standalone unit tests for src/descriptor.c / src/libltntstools/descriptor.h.
 * Builds directly against ../src/descriptor.c, no other library dependencies
 * (only libc: memcpy/memcmp/strlen).
 *
 * NOTE: every function in descriptor.c that takes a
 * `struct ltntstools_descriptor_list_s *list` will crash on a NULL list --
 * including ltntstools_descriptor_list_add(), which reads `list->count` on
 * its very first line, BEFORE its own `if (!list) return -1;` check further
 * down (that check is unreachable dead code as a result). None of the
 * `_contains_*` scanner functions check for NULL at all. This file
 * deliberately does not pass NULL to any of them, since that would crash
 * the test binary rather than fail a test.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "libltntstools/descriptor.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

static void list_add(struct ltntstools_descriptor_list_s *list, uint8_t tag, const void *data, uint8_t len)
{
	int ret = ltntstools_descriptor_list_add(list, tag, (uint8_t *)data, len);
	CHECK(ret == 0);
}

/* -------- ltntstools_descriptor_tag_description() -------- */

static void test_tag_description_known_values(void)
{
	CHECK(strcmp(ltntstools_descriptor_tag_description(0x00), "Reserved") == 0);
	CHECK(strcmp(ltntstools_descriptor_tag_description(0x1b), "MPEG-4 video descriptor") == 0);
	CHECK(strcmp(ltntstools_descriptor_tag_description(0x6a), "AC-3 descriptor (DVB)") == 0);
	CHECK(strcmp(ltntstools_descriptor_tag_description(0xcc), "E-AC-3 audio stream descriptor (ATSC A/65)") == 0);
}

static void test_tag_description_reserved_range(void)
{
	/* < 0x40 and not explicitly listed in the table. */
	CHECK(strcmp(ltntstools_descriptor_tag_description(0x37), "ISO/IEC 13818-1 reserved") == 0);
	CHECK(strcmp(ltntstools_descriptor_tag_description(0x3e), "ISO/IEC 13818-1 reserved") == 0);
}

static void test_tag_description_vendor_private_range(void)
{
	/* Explicit ATSC entries inside 0xa0-0xaf must still return their real description. */
	CHECK(strcmp(ltntstools_descriptor_tag_description(0xa1), "Service location descriptor (ATSC A/65)") == 0);

	/* Gaps inside 0xa0-0xaf not explicitly listed fall back to the range default. */
	CHECK(strcmp(ltntstools_descriptor_tag_description(0xa4), "Vendor/private descriptor") == 0);
	CHECK(strcmp(ltntstools_descriptor_tag_description(0xaf), "Vendor/private descriptor") == 0);
}

static void test_tag_description_user_private_range(void)
{
	/* >= 0x40, not in the table, and outside 0xa0-0xaf. */
	CHECK(strcmp(ltntstools_descriptor_tag_description(0x85), "User Private") == 0);
	CHECK(strcmp(ltntstools_descriptor_tag_description(0xff), "User Private") == 0);
}

static void test_tag_description_never_null_for_all_256_values(void)
{
	for (int t = 0; t <= 0xff; t++) {
		if (ltntstools_descriptor_tag_description((uint8_t)t) == NULL) {
			fprintf(stderr, "FAIL: tag_description(0x%02x) returned NULL\n", t);
			g_failures++;
		}
	}
}

/* -------- ltntstools_descriptor_list_add() -------- */

static void test_add_single_entry_stores_fields(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[3] = { 0xAA, 0xBB, 0xCC };

	CHECK(ltntstools_descriptor_list_add(&list, 0x42, data, sizeof(data)) == 0);
	CHECK(list.count == 1);
	CHECK(list.array[0].tag == 0x42);
	CHECK(list.array[0].len == 3);
	CHECK(memcmp(list.array[0].data, data, 3) == 0);
}

static void test_add_zero_length_entry(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t dummy = 0;

	CHECK(ltntstools_descriptor_list_add(&list, 0x10, &dummy, 0) == 0);
	CHECK(list.count == 1);
	CHECK(list.array[0].len == 0);
}

static void test_add_max_length_entry(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[255];
	for (int i = 0; i < 255; i++) {
		data[i] = (uint8_t)i;
	}

	CHECK(ltntstools_descriptor_list_add(&list, 0x20, data, 255) == 0);
	CHECK(list.array[0].len == 255);
	CHECK(memcmp(list.array[0].data, data, 255) == 0);
}

static void test_add_rejects_null_src(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	CHECK(ltntstools_descriptor_list_add(&list, 0x10, NULL, 4) < 0);
	CHECK(list.count == 0);
}

static void test_add_fills_to_capacity_then_rejects(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[1] = { 0x01 };

	for (int i = 0; i < LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX; i++) {
		CHECK(ltntstools_descriptor_list_add(&list, (uint8_t)i, data, 1) == 0);
	}
	CHECK(list.count == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX);

	/* One more must be rejected, and must not disturb the existing entries. */
	CHECK(ltntstools_descriptor_list_add(&list, 0xEE, data, 1) < 0);
	CHECK(list.count == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX);
	CHECK(list.array[0].tag == 0);
	CHECK(list.array[LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX - 1].tag == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX - 1);
}

static void test_add_multiple_entries_preserve_order_and_content(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t a[2] = { 1, 2 };
	uint8_t b[3] = { 3, 4, 5 };

	list_add(&list, 0x0a, a, sizeof(a));
	list_add(&list, 0x0b, b, sizeof(b));

	CHECK(list.count == 2);
	CHECK(list.array[0].tag == 0x0a && list.array[0].len == 2 && memcmp(list.array[0].data, a, 2) == 0);
	CHECK(list.array[1].tag == 0x0b && list.array[1].len == 3 && memcmp(list.array[1].data, b, 3) == 0);
}

/* -------- contains_smpte2064_registration (LIPS) -------- */

static void test_contains_smpte2064_found(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "LIPS", 4);
	CHECK(ltntstools_descriptor_list_contains_smpte2064_registration(&list) == 1);
}

static void test_contains_smpte2064_not_found_wrong_tag(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x06, "LIPS", 4);
	CHECK(ltntstools_descriptor_list_contains_smpte2064_registration(&list) == 0);
}

static void test_contains_smpte2064_not_found_wrong_content(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "LIPX", 4);
	CHECK(ltntstools_descriptor_list_contains_smpte2064_registration(&list) == 0);
}

/* -------- contains_scte35_cue_registration (CUEI) -------- */

static void test_contains_scte35_found(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "CUEI", 4);
	CHECK(ltntstools_descriptor_list_contains_scte35_cue_registration(&list) == 1);
}

static void test_contains_scte35_not_found(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "CUEX", 4);
	CHECK(ltntstools_descriptor_list_contains_scte35_cue_registration(&list) == 0);
}

/* -------- contains_video_av1_registration (AV01, len >= 4) -------- */

static void test_contains_av1_found_exact_len(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "AV01", 4);
	CHECK(ltntstools_descriptor_list_contains_video_av1_registration(&list) == 1);
}

static void test_contains_av1_found_with_trailing_bytes(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "AV01X", 5);
	CHECK(ltntstools_descriptor_list_contains_video_av1_registration(&list) == 1);
}

static void test_contains_av1_not_found_too_short(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "AV0", 3);
	CHECK(ltntstools_descriptor_list_contains_video_av1_registration(&list) == 0);
}

/* -------- contains_teletext -------- */

static void test_contains_teletext_found(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[5] = { 'e', 'n', 'g', 0x01, 0x23 };
	list_add(&list, 0x56, data, 5);
	CHECK(ltntstools_descriptor_list_contains_teletext(&list) == 1);
}

static void test_contains_teletext_not_found_wrong_len(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[4] = { 'e', 'n', 'g', 0x01 };
	list_add(&list, 0x56, data, 4);
	CHECK(ltntstools_descriptor_list_contains_teletext(&list) == 0);
}

/* -------- contains_smpte2038_registration (3 alternative matches) -------- */

static void test_contains_smpte2038_found_via_c4_tag_full_string(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0xc4, "SMPTE2038", 9);
	CHECK(ltntstools_descriptor_list_contains_smpte2038_registration(&list) == 1);
}

static void test_contains_smpte2038_found_via_05_tag_vanc(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "VANC", 4);
	CHECK(ltntstools_descriptor_list_contains_smpte2038_registration(&list) == 1);
}

static void test_contains_smpte2038_found_via_c4_empty(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t dummy = 0;
	list_add(&list, 0xc4, &dummy, 0);
	CHECK(ltntstools_descriptor_list_contains_smpte2038_registration(&list) == 1);
}

static void test_contains_smpte2038_not_found(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	list_add(&list, 0x05, "LIPS", 4); /* unrelated registration */
	CHECK(ltntstools_descriptor_list_contains_smpte2038_registration(&list) == 0);
}

/* -------- contains_ltn_encoder_sw_version -------- */

static void test_contains_ltn_sw_version_found_extracts_values(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[4] = { 0x01, 3, 14, 159 }; /* sub-tag 1 = sw version, major.minor.patch */
	list_add(&list, 0xa2, data, 4);

	unsigned int major = 0, minor = 0, patch = 0;
	CHECK(ltntstools_descriptor_list_contains_ltn_encoder_sw_version(&list, &major, &minor, &patch) == 1);
	CHECK(major == 3 && minor == 14 && patch == 159);
}

static void test_contains_ltn_sw_version_not_found_wrong_subtag(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[4] = { 0x02, 3, 14, 159 }; /* wrong embedded sub-tag */
	list_add(&list, 0xa2, data, 4);

	unsigned int major = 0, minor = 0, patch = 0;
	CHECK(ltntstools_descriptor_list_contains_ltn_encoder_sw_version(&list, &major, &minor, &patch) == 0);
}

/* -------- contains_iso639_audio_descriptor -------- */

static void test_contains_iso639_found_extracts_values(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[4] = { 'e', 'n', 'g', 0x03 };
	list_add(&list, 0x0a, data, 4);

	unsigned char lang[4] = { 0 };
	unsigned int type = 0;
	CHECK(ltntstools_descriptor_list_contains_iso639_audio_descriptor(&list, lang, &type) == 1);
	CHECK(memcmp(lang, "eng", 3) == 0);
	CHECK(type == 3);
}

static void test_contains_iso639_not_found(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };
	uint8_t data[3] = { 'e', 'n', 'g' }; /* wrong length */
	list_add(&list, 0x0a, data, 3);

	unsigned char lang[4] = { 0 };
	unsigned int type = 0;
	CHECK(ltntstools_descriptor_list_contains_iso639_audio_descriptor(&list, lang, &type) == 0);
}

/* -------- each scanner finds only its own descriptor in a mixed list -------- */

static void test_mixed_list_each_scanner_finds_only_its_own_descriptor(void)
{
	struct ltntstools_descriptor_list_s list = { 0 };

	list_add(&list, 0x0a, "eng\x06", 4);   /* iso639 */
	list_add(&list, 0x05, "CUEI", 4);      /* scte35 */
	list_add(&list, 0x56, "\x01\x02\x03\x04\x05", 5); /* teletext */
	list_add(&list, 0xa2, "\x01\x02\x03\x04", 4);      /* ltn sw version */

	CHECK(list.count == 4);

	CHECK(ltntstools_descriptor_list_contains_scte35_cue_registration(&list) == 1);
	CHECK(ltntstools_descriptor_list_contains_smpte2064_registration(&list) == 0); /* LIPS not present */
	CHECK(ltntstools_descriptor_list_contains_teletext(&list) == 1);
	CHECK(ltntstools_descriptor_list_contains_video_av1_registration(&list) == 0);
	CHECK(ltntstools_descriptor_list_contains_smpte2038_registration(&list) == 0);

	unsigned int major = 0, minor = 0, patch = 0;
	CHECK(ltntstools_descriptor_list_contains_ltn_encoder_sw_version(&list, &major, &minor, &patch) == 1);
	CHECK(major == 2 && minor == 3 && patch == 4);

	unsigned char lang[4] = { 0 };
	unsigned int type = 0;
	CHECK(ltntstools_descriptor_list_contains_iso639_audio_descriptor(&list, lang, &type) == 1);
	CHECK(memcmp(lang, "eng", 3) == 0);
	CHECK(type == 6);
}

int main(void)
{
	test_tag_description_known_values();
	test_tag_description_reserved_range();
	test_tag_description_vendor_private_range();
	test_tag_description_user_private_range();
	test_tag_description_never_null_for_all_256_values();

	test_add_single_entry_stores_fields();
	test_add_zero_length_entry();
	test_add_max_length_entry();
	test_add_rejects_null_src();
	test_add_fills_to_capacity_then_rejects();
	test_add_multiple_entries_preserve_order_and_content();

	test_contains_smpte2064_found();
	test_contains_smpte2064_not_found_wrong_tag();
	test_contains_smpte2064_not_found_wrong_content();

	test_contains_scte35_found();
	test_contains_scte35_not_found();

	test_contains_av1_found_exact_len();
	test_contains_av1_found_with_trailing_bytes();
	test_contains_av1_not_found_too_short();

	test_contains_teletext_found();
	test_contains_teletext_not_found_wrong_len();

	test_contains_smpte2038_found_via_c4_tag_full_string();
	test_contains_smpte2038_found_via_05_tag_vanc();
	test_contains_smpte2038_found_via_c4_empty();
	test_contains_smpte2038_not_found();

	test_contains_ltn_sw_version_found_extracts_values();
	test_contains_ltn_sw_version_not_found_wrong_subtag();

	test_contains_iso639_found_extracts_values();
	test_contains_iso639_not_found();

	test_mixed_list_each_scanner_finds_only_its_own_descriptor();

	if (g_failures == 0) {
		printf("PASS: all descriptor tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d descriptor test(s) failed\n", g_failures);
	return 1;
}
