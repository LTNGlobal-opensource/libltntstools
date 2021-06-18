#ifndef TR101290_DESCRIPTOR_H
#define TR101290_DESCRIPTOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This needs to be a single static allocation, don't add pointers
 * else partent structure that rely on shallow copy will become
 * unstable.
 */
struct ltntstools_descriptor_entry_s
{
	uint8_t tag;
	uint8_t len;
	uint8_t data[256];
};

struct ltntstools_descriptor_list_s
{
	uint32_t count;
#define LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX 16
	struct ltntstools_descriptor_entry_s array[LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX];
};

int ltntstools_descriptor_list_add(struct ltntstools_descriptor_list_s *list, uint8_t tag, uint8_t *src, uint8_t lengthBytes);

#ifdef __cplusplus
};
#endif

#endif /* TR101290_DESCRIPTOR_H */
