/**
 * @file        tr101290.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020 LTN Global,Inc. All Rights Reserved.
 * @brief       A module to examine ISO13818 transport packets for priority 1 and 2 errors, raising
 *              and clearing alarm conditions accordingly. A threaded high performance design
 *              designed to be instantiated multiple times within a single process, with a non-blocking API.
 *              See ETSI TR101290 v1.2.1 (2001-05)
 */

#ifndef _TR101290_H
#define _TR101290_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tables 5.2.1, 5.2.2, and 5.2.3 */
enum ltntstools_tr101290_event_e
{
	E101290_UNDEFINED = 0,

	/* Priority 1 */
	E101290_P1_1__TS_SYNC_LOSS,
	E101290_P1_2__SYNC_BYTE_ERROR,
	E101290_P1_3__PAT_ERROR,
	E101290_P1_3a__PAT_ERROR_2,
	E101290_P1_4__CONTINUITY_COUNTER_ERROR,
	E101290_P1_5__PMT_ERROR,
	E101290_P1_5a__PMT_ERROR_2,
	E101290_P1_6__PID_ERROR,

	/* Priority 2 */
	E101290_P2_1__TRANSPORT_ERROR,
	E101290_P2_2__CRC_ERROR,
	E101290_P2_3__PCR_ERROR,
	E101290_P2_3a__PCR_REPETITION_ERROR,
	E101290_P2_4__PCR_ACCURACY_ERROR,
	E101290_P2_5__PTS_ERROR,
	E101290_P2_6__CAT_ERROR,

	/* Third Priority: Application Dependant Monitoring */
	/* Not supported. */

	E101290_MAX,
};

struct ltntstools_tr101290_alarm_s
{
	enum ltntstools_tr101290_event_e id;

	int priorityNr;
	struct timeval timestamp;
	int raised;

	char description[256];
};

typedef void (*ltntstools_tr101290_notification)(void *userContext, struct ltntstools_tr101290_alarm_s *array, int count);

/**
 * @brief       Allocate a TR101290 module handle for state tracking purposes, pass this handle to other calls.
 *              Free this handle with a call to ltntstools_tr101290_free().
 * @param[out]  void **hdl - Handle returned to the caller.
 * @param[in]   ltntstools_tr101290_notification cb_notify - User supplied callback where alarms are posted to.
 * @param[in]   void *userContext - User supplied opaque context, passed during callback calls.
 * @return      0 on success else < 0.
 */
int     ltntstools_tr101290_alloc(void **hdl, ltntstools_tr101290_notification cb_notify, void *userContext);

/**
 * @brief       Free a previously allocated TR101290 module handle. Caller must make no attempt to use the
 *              handle in any other call, once this function has returned.
 * @param[in]   void *hdl - Object to be released.
 */
void    ltntstools_tr101290_free(void *hdl);

/**
 * @brief       Write one or more consecutive ISO13818 188 byte transport packets into the TR101290 module
 *              for inspection and analysis.
 *              Analysis of these packets is deferred and processed by a background thread.
 *              Notifications/Errors generated as a result of these packets are passed to your application
 *              via the ltntstools_tr101290_event_callback mechanism.
 * @param[in]   void **hdl - Handle returned to the caller.
 * @return      The number of packets inspected, or < 0 on error.
 */
ssize_t ltntstools_tr101290_write(void *hdl, const uint8_t *buf, size_t packetCount);

const char *ltntstools_tr101290_event_name_ascii(enum ltntstools_tr101290_event_e event);
int ltntstools_tr101290_event_priority(enum ltntstools_tr101290_event_e event);

int ltntstools_tr101290_event_clear(void *hdl, enum ltntstools_tr101290_event_e event);

int ltntstools_tr101290_event_processing_disable(void *hdl, enum ltntstools_tr101290_event_e event);
int ltntstools_tr101290_event_processing_enable(void *hdl, enum ltntstools_tr101290_event_e event);
int ltntstools_tr101290_event_processing_disable_all(void *hdl);
int ltntstools_tr101290_event_processing_enable_all(void *hdl);

#ifdef __cplusplus
};
#endif

#endif /* _TR101290_H */


