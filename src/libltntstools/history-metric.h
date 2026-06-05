#ifndef LIBLTNTSTOOLS_HISTORY_METRIC_H
#define LIBLTNTSTOOLS_HISTORY_METRIC_H

#include <libltntstools/xorg-list.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

/**
 * @file        history-metric.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2026 LTN Global,Inc. All Rights Reserved.
 * @brief       Track a generic metric over time, where each second a new value is applied.
 *              The framework doesn't want increment values, it wants instances of a value.
 *              For example, don't give it a cc_count, give it an new instance of a cc_error.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct ltntstools_history_metric_s
{
	struct xorg_list list;
	time_t ts;   /**< Epoch minute this bucket represents */
	uint64_t count;  /**< A count of something  */
};

struct ltntstools_history_metric_collection_s
{
	pthread_mutex_t lock;
	struct xorg_list list;
	const char *name;
};

/**
 * @brief       Allocate a framework context capable of demuxing and parsing PES streams.
 * @param[in]   const char * - Human readable name
 * @return      struct on success, else NULL.
 */
struct ltntstools_history_metric_collection_s *ltntstools_history_metric_collection_alloc(const char *name);

/**
 * @brief       Initialize context capable of demuxing and parsing PES streams.
 * @param[in]   struct ltntstools_history_metric_collection_s * - collection struct
 * @param[in]   const char * - Human readable name
 * @return      0 on success, else < 0
 */
int ltntstools_history_metric_collection_init(struct ltntstools_history_metric_collection_s *c, const char *name);

/**
 * @brief       Free a previously allocated collection, and all of its associated metrics.
 * @param[in]   struct ltntstools_history_metric_collection_s * - collection struct
 */
void ltntstools_history_metric_collection_free(struct ltntstools_history_metric_collection_s *c);

/**
 * @brief       Add a previously allocated metric to a collection. See ltntstools_history_metric_alloc().
 * @param[in]   struct ltntstools_history_metric_collection_s * - collection struct
 * @param[in]   struct ltntstools_history_metric_s * - metric
 */
void ltntstools_history_metric_collection_add(struct ltntstools_history_metric_collection_s *c,
	struct ltntstools_history_metric_s *m);

/**
 * @brief       Remove all metrics, effectively wipe all history.
 * @param[in]   struct ltntstools_history_metric_collection_s * - collection struct
 */
void ltntstools_history_metric_collection_reset(struct ltntstools_history_metric_collection_s *c);

/**
 * @brief       Sum up the counts over the 1hr time period
 * @param[in]   struct ltntstools_history_metric_collection_s * - collection struct
 * @param[out]  uint64_t * - count for that time period
 * @return      0 on success, else < 0
 */
int ltntstools_history_metric_collection_count_until_1hr(struct ltntstools_history_metric_collection_s *c, uint64_t *count);

/**
 * @brief       Sum up the counts over the 24hr time period
 * @param[in]   struct ltntstools_history_metric_collection_s * - collection struct
 * @param[out]  uint64_t * - count for that time period
 * @return      0 on success, else < 0
 */
int ltntstools_history_metric_collection_count_until_24hr(struct ltntstools_history_metric_collection_s *c, uint64_t *count);

/**
 * @brief       Sum up the counts between now and user defined time.
 * @param[in]   struct ltntstools_history_metric_collection_s * - collection struct
 * @param[in]   time_t - window
 * @param[out]  uint64_t * - count for that time period
 * @return      0 on success, else < 0
 */
int ltntstools_history_metric_collection_count_until(struct ltntstools_history_metric_collection_s *c, time_t window, uint64_t *count);

/**
 * @brief       Allocate a framework context capable of demuxing and parsing PES streams.
 * @param[in]   void **hdl - Handle / context for further use.
 * @param[in]   uint16_t pid - MPEG TS transport PID to be de-muxed
 * @param[in]   vbv_callback cb - user supplied callback for PES frame delivery
 * @param[in]   void *userContext - user private context, passed back to caller during callback.
 * @param[in]   struct vbv_decoder_profile_s * - Expected decoder profile
 * @return      0 on success, else < 0.
 */
struct ltntstools_history_metric_s *ltntstools_history_metric_alloc(time_t now, uint64_t value);

/**
 * @brief       Free a previously allocated context.
 * @param[in]   void *hdl - Handle / context.
 */
void ltntstools_history_metric_free(struct ltntstools_history_metric_s *m);

#ifdef __cplusplus
};
#endif

#endif /* LIBLTNTSTOOLS_HISTORY_METRIC_H */
