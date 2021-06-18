
#include <stdio.h>
#include "libltntstools/ltntstools.h"

int ltntstools_descriptor_list_add(struct ltntstools_descriptor_list_s *list,
	uint8_t tag, uint8_t *src, uint8_t lengthBytes)
{
	if (list->count == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX)
		return -1;

	if (lengthBytes > 256)
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

