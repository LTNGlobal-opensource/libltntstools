#include "streammodel-types.h"

#define LOCAL_DEBUG 0

void extractors_free(struct streammodel_ctx_s *ctx)
{
	if (!ctx->enableSectionCRCChecks)
		return;

	for (int i = 0; i < ctx->seCount; i++) {
		if (ctx->seArray) {
			struct se_array_item_s *i = ctx->seArray + ctx->seCount;

			if (i->hdl)
				ltntstools_sectionextractor_free(i->hdl);

			if (i->name)
				free(i->name);
		}
	}
}

int extractors_add(struct streammodel_ctx_s *ctx, uint16_t pid, uint8_t tableId, char *name, uint32_t context)
{
	/* The streammodel api will call us constantly to add PMT pids every time the
	 * its stream model is refreshed. We don't want to create new extractors for the
	 * same pids. Lookup PMT specific pids and re-use.
	 */
	if (tableId == 02 /* PMT */) {
		for (int i = 9; i < ctx->seCount; i++) {
			struct se_array_item_s *item = ctx->seArray + i;
			if (item->pid == pid)
				return 0; /* Success, already in the list */
		}
	}

#if LOCAL_DEBUG
	printf("%s() Adding %s\n", __func__, name);
#endif
	ctx->seArray = realloc(ctx->seArray, sizeof(struct se_array_item_s) * (ctx->seCount + 1));
	struct se_array_item_s *i = ctx->seArray + ctx->seCount;

	i->pid = pid;
	i->tableId = tableId;
	i->name = strdup(name);
	i->context = context;

	int ret = ltntstools_sectionextractor_alloc(&i->hdl, i->pid, i->tableId);
	if (ret < 0) {
		fprintf(stderr, "Failed to add section extarctor\n");
		ctx->seCount--;
		ctx->seArray = realloc(ctx->seArray, sizeof(struct se_array_item_s) * ctx->seCount);
		return ret;
	}
	ctx->seCount++;

	return 0; /* Success */
}

/* For TR101290, we need to track CRC issues with certain tables.
 * for those that have fixed pids, take care of them here.
 * For those on varibale pids, we'll handle them in a different
 * function.
 * 	  PID: Desc   Section#
 *   0  PAT    00
 * var  CAT    01
 * var  PMT    02
 *  10  NIT    40
 *  11  BAT    4A
 *  11  SDT    42
 *  14  TOT    73
 *  12  EIT    4E 4F 5F 6F
 */

int extractors_alloc(struct streammodel_ctx_s *ctx)
{
	if (!ctx->enableSectionCRCChecks)
		return 0;

	/* Static list of all DVB tables. */
	/* Don't add anything between here.... */
	extractors_add(ctx, 0x00, 0x00, "PAT", STREAMMODEL_CB_CONTEXT_PAT); /* PAT */
	extractors_add(ctx, 0x10, 0x40, "NIT", STREAMMODEL_CB_CONTEXT_NIT); /* NIT */
	extractors_add(ctx, 0x11, 0x4A, "BAT", STREAMMODEL_CB_CONTEXT_BAT); /* BAT */
	extractors_add(ctx, 0x11, 0x42, "SDT", STREAMMODEL_CB_CONTEXT_SDT); /* SDT */
	extractors_add(ctx, 0x12, 0x4E, "EIT", STREAMMODEL_CB_CONTEXT_EIT); /* EIT */
	extractors_add(ctx, 0x12, 0x4F, "EIT", STREAMMODEL_CB_CONTEXT_EIT); /* EIT */
	extractors_add(ctx, 0x12, 0x5F, "EIT", STREAMMODEL_CB_CONTEXT_EIT); /* EIT */
	extractors_add(ctx, 0x12, 0x6F, "EIT", STREAMMODEL_CB_CONTEXT_EIT); /* EIT */
	extractors_add(ctx, 0x14, 0x73, "TOT", STREAMMODEL_CB_CONTEXT_TOT); /* TOT */
	/* ... end here without adjusting the _write swtch table hard-coded indexes. */

	return 0; /* Success */
}

static void _write_pkt(struct streammodel_ctx_s *ctx, struct se_array_item_s *item, const uint8_t *pkt)
{
	ltntstools_sectionextractor_write(item->hdl, pkt, 1, &item->complete, &item->crcValid);
	if (item->complete) {
		if (ctx->cb) {
			struct streammodel_callback_args_s args;

			args.status  = STREAMMODEL_CB_CRC_STATUS;
			args.context = item->context;
			args.ptr     = NULL;
			args.arg     = item->crcValid;

			ctx->cb(ctx->userContext, &args);
		}
	}

#if LOCAL_DEBUG
	if (item->complete && item->crcValid == 0) {
		printf("SE [0x%04x:%02x %s] complete crcValid %d\n", item->pid, item->tableId, item->name, item->crcValid);
	}
#endif
}

int extractors_write(struct streammodel_ctx_s *ctx, const uint8_t *pkts, int packetCount)
{
	struct se_array_item_s *item = NULL;

	for (int i = 0; i < packetCount; i++) {
		const uint8_t *pkt = &pkts[i * 188];
		uint16_t pid = ltntstools_pid(pkt);
		switch (pid) {
		case 0x00:
			item = ctx->seArray + 0; /* PAT */
			_write_pkt(ctx, item, pkt);
			break;
		case 0x10:
			item = ctx->seArray + 1; /* NIT */
			_write_pkt(ctx, item, pkt);
			break;
		case 0x11:
			item = ctx->seArray + 2; /* BAT */
			_write_pkt(ctx, item, pkt);
			item = ctx->seArray + 3; /* SDT */
			_write_pkt(ctx, item, pkt);
			break;
		case 0x12:
			item = ctx->seArray + 4; /* EIT */
			_write_pkt(ctx, item, pkt);
			item = ctx->seArray + 5; /* EIT */
			_write_pkt(ctx, item, pkt);
			item = ctx->seArray + 6; /* EIT */
			_write_pkt(ctx, item, pkt);
			item = ctx->seArray + 7; /* EIT */
			_write_pkt(ctx, item, pkt);
			break;
		case 0x14:
			item = ctx->seArray + 8; /* TOT */
			_write_pkt(ctx, item, pkt);
			break;
		default:
			for (int i = 9; i < ctx->seCount; i++ ) {
				item = ctx->seArray + i;
				_write_pkt(ctx, item, pkt);
			}
		}
	}

	return packetCount;
}

#if 0
	/* Initialize base section extractors if they're not yet initialized. */
	if (ctx->se_nit == NULL) {
		ret = ltntstools_sectionextractor_alloc(&ctx->se_nit, 0x10, 0x40);
		if (ret < 0) {

		}

	}
#endif
