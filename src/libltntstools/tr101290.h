/**
 * @file        tr101290.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020 LTN Global,Inc. All Rights Reserved.
 * @brief       A module to examine ISO13818 transport packets for priority 1 and 2 errors, raising
 *              and clearing alarm conditions accordingly. A threaded high performance design
 *              designed to be instantiated multiple times within a single process, with a non-blocking API.
 *              See ETSI TR101290 v1.2.1 (2001-05)
 */

/* Usage:
 * 1. Setup your application callback, typed from ltntstools_tr101290_notification.
 *      int     ltntstools_tr101290_alloc(void **hdl, ltntstools_tr101290_notification cb_notify, void *userContext);
 * 2. Write the entire transport mux to the layer.
 *      ssize_t ltntstools_tr101290_write(void *hdl, const uint8_t *buf, size_t packetCount);
 * 3. Free the context once you're done using the functionality.
 *      ltntstools_tr101290_free(void *hdl);
*/

#ifndef _TR101290_H
#define _TR101290_H

#include <time.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tables 5.2.1, 5.2.2, and 5.2.3 */
/**
 * @brief  All of the potential event type that the framework supports.
 *         See the TR101290 recommednation document.
 */
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

	/* Priority 4 */
	E101290_P4_1__UDP_DROPS,

	/* Third Priority: Application Dependant Monitoring */
	/* Not supported. */

	E101290_MAX,
};

/**
 * @brief       A fully defined alarm, passed to the caller via notification.
 */
struct ltntstools_tr101290_alarm_s
{
	enum   ltntstools_tr101290_event_e id;  /**< Eg. E101290_P1_1__TS_SYNC_LOSS */
	int    priorityNr;                      /**< TR101290 formal Priority level. IE. 1, 2 or 3 */
	struct timeval timestamp;               /**< timestamp of the notification */
	int    raised;                          /**< Boolean. 0 alarm clear, 1 raised. */
	char   description[256];                /**< Text description of the alarm */
	char   arg[128];                        /**< Some alarms have args, here in text form. */
};

/**
 * @brief       Helper function. Print to a file descriptor the contents of the alarm.
 * @param[in]   int - file descriptor
 * @param[in]   struct ltntstools_tr101290_alarm_s * - alarm
 */
void ltntstools_tr101290_event_dprintf(int fd, struct ltntstools_tr101290_alarm_s *alarm);

/**
 * @brief       User specific callback for TR101290 notifications.
 *              When the framework calls your callback, you own the array and are responsible for its destruction.
 */
typedef void (*ltntstools_tr101290_notification)(void *userContext, struct ltntstools_tr101290_alarm_s *array, int count);

/**
 * @brief       Allocate a TR101290 module handle for state tracking purposes, pass this handle to other calls.
 *              Free this handle with a call to ltntstools_tr101290_free().
 * @param[out]  void **hdl - Handle returned to the caller.
 * @param[in]   ltntstools_tr101290_notification cb_notify - User supplied callback where alarms are posted to.
 *              When the framework calls your callback, you own the array and are responsible for its destruction.
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
 * @param[in]   void *hdl - Handle returned to the caller.
 * @return      The number of packets inspected, or < 0 on error.
 */
ssize_t ltntstools_tr101290_write(void *hdl, const uint8_t *buf, size_t packetCount, struct timeval *timestamp);

/**
 * @brief       Convert an event id into a human readable string.
 *              The function will always return a string, regardless of input.
 * @param[in]   enum ltntstools_tr101290_event_e - event
 * @return      const char * - label
 */
const char *ltntstools_tr101290_event_name_ascii(enum ltntstools_tr101290_event_e event);

/**
 * @brief       Query the TR101290 formal priority assigned to a specific event object.
 * @param[in]   enum ltntstools_tr101290_event_e - event
 * @return      int - priority   (Typically 1, 2 or 3)
 */
int ltntstools_tr101290_event_priority(enum ltntstools_tr101290_event_e event);

/**
 * @brief       Attempt to clear a specific alarm/event.
 *              The internal TR101290 statemachine will move the event back to raise if the
 *              event truly hasn't cleared. Generally you don't need to use the clear or
 *              raise APIs directly.
 * @param[in]   void *hdl - Handle returned to the caller.
 * @param[in]   enum ltntstools_tr101290_event_e - event
 * @return      0 on success, else < 0.
 */
int ltntstools_tr101290_event_clear(void *hdl, enum ltntstools_tr101290_event_e event);

/**
 * @brief       Permit the application to disable specific events.
 *              An example of this may be to disable CAT events when operating in the US.
 *              These events will no longer be monitored.
 * @param[in]   void **hdl - Handle returned to the caller.
 * @return      0 on success, else error.
 */
int ltntstools_tr101290_event_processing_disable(void *hdl, enum ltntstools_tr101290_event_e event);

/**
 * @brief       Permit the application to enable specific events, including those unsupported
 *              events that could bring false hope. Thin ice, tread carefully.
 *              You generally don't want to use this API.
 * @param[in]   void **hdl - Handle returned to the caller.
 * @return      0 on success, else error.
 */
int ltntstools_tr101290_event_processing_enable(void *hdl, enum ltntstools_tr101290_event_e event);

/**
 * @brief       Permit the application to disable all events.
 * @param[in]   void **hdl - Handle returned to the caller.
 * @return      0 on success, else error.
 */
int ltntstools_tr101290_event_processing_disable_all(void *hdl);

/**
 * @brief       Permit the application to enable all events, including those unsupported.
 * @param[in]   void **hdl - Handle returned to the caller.
 * @return      0 on success, else error.
 */
int ltntstools_tr101290_event_processing_enable_all(void *hdl);

/**
 * @brief       The summary of a current alarm, in structure form.
 */
struct ltntstools_tr101290_summary_item_s
{
	enum ltntstools_tr101290_event_e id;

	int enabled;	             /**< Boolean. Is the stat actively being monitored. */
	int priorityNr;              /**< TR101290 formal Priority level. IE. 1, 2 or 3 */
	struct timeval last_update;  /**< timestamp of the notification */
	int raised;	                 /**< Boolean. 0 alarm clear, 1 raised. */
	char arg[128];               /**< Some alarms have args, here in text form. */
};
void ltntstools_tr101290_summary_item_dprintf(int fd, struct ltntstools_tr101290_summary_item_s *summary_item);

/**
 * @brief       Return a summary of each event, regardless of whether the caller has enabled or disabled its reporting
 *              capability, and return the state of each event (clear or raised), and the last date this condition changed.
 *              This is considered a "polling helper" function, for simple application that don't want to track state
 *              and handle that complexity in their nominated callback notification handler.
 * @param[out]  void *hdl - Handle returned to the caller.
 * @param[in]   struct ltntstools_tr101290_alarm_s **items - Array of items returned, caller must free the allocation
 * @param[in]   int itemCount - Number of items in the array
 * @return      0 on success else < 0.
 */
int ltntstools_tr101290_summary_get(void *hdl, struct ltntstools_tr101290_summary_item_s **item, int *itemCount);

/**
 * @brief       Produce a sumamry txt report of all events and statud to a file descriptor.
 * @param[out]  void *hdl - Handle returned to the caller.
 * @param[in]   int - file descriptor
 * @return      0 on success else < 0.
 */
int ltntstools_tr101290_summary_report_dprintf(void *hdl, int fd);

/**
 * @brief       Enable the creation of an alarm logfile, user supplied name.
 *              The log will include the raising and clearing of alarms, as they occur.
 *              Existing logs will be appended to on startup.
 * @param[out]  void *hdl - Handle returned to the caller.
 * @param[in]   const char *afname - Absolute filename that will be created or appended to.
 * @return      0 on success else < 0.
 */
int ltntstools_tr101290_log_enable(void *hdl, const char *afname);

/**
 * @brief       Close the current logfile and rotate out.
 *              NOT YET SUPPORTED, WILL RETURN ERROR.
 * @param[out]  void *hdl - Handle returned to the caller.
 * @return      0 on success else < 0.
 */
int ltntstools_tr101290_log_rotate(void *hdl);

/**
 * @brief       Place all alarms into the raised state, knowing that if the statemachines
 *              for analyzing conditions are working properly, these alarms will be subsequently
 *              placed into a 'clear' condition.
 * @param[out]  void *hdl - Handle returned to the caller.
 * @return      0 on success else < 0.
 */
int ltntstools_tr101290_reset_alarms(void *hdl);

#ifdef __cplusplus
};
#endif

#endif /* _TR101290_H */


