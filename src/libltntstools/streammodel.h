#ifndef STREAMMODEL_H
#define STREAMMODEL_H

/**
 * @file        streammodel.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2020-2022 LTN Global,Inc. All Rights Reserved.
 * @brief       A module to examine ISO13818 transport packets, extact PAT/PMT information
 *              with just a few calls. The stream model is designed to constantly parse the stream
 *              in an efficient an memory leak/free way, so its safe to constantly feed the api
 *              7x24 fully formed transport streams and monitor for changes to service information.
 * 
 * Usage:
 *              Typically an application will allocate a context, feed packets into the frame and
 *              monitor the _write 'complete' result. When this goes high (1), it's safe to
 *              query the model _query_model() until the next _write call takes place.
 *              The caller of _query_model is responsible for freeing any resulting pat object.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libltntstools/ltntstools.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief         Allocate a context for use with other streammodel api calls.
 * @param[out]    void **hdl - Buffer of data, possibly containing none or more NAL packets.
 * @return        0 - Success
 * @return      < 0 - Error
 */
int ltntstools_streammodel_alloc(void **hdl, void *userContext);

/**
 * @brief         Single call that opens a transport fiel, performs all the queries and returns a stream model object.
 * @param[in]     const char *url - file to open, url to open, we support mode ffmpeg input urls.
 * @param[out]    struct ltntstools_pat_s **pat - returned object, or NULL.
 * @return        0 - Success
 * @return      < 0 - Error
 */
int ltntstools_streammodel_alloc_from_url(const char *url, struct ltntstools_pat_s **pat);

/**
 * @brief         Free a previously allocated context. Don't attempt to use the context after its freed.
 * @param[in]     void *hdl - Previously allocate context handle.
 */
void ltntstools_streammodel_free(void *hdl);

/**
 * @brief         Write a buffer of properly aligned transport packets into the framework.
 *                When the frameowrk has processed enough of the packets to fully construct
 *                a PAT/PMT tree, the complete result will contain true.
 * @param[in]     void *hdl - Previously allocate context handle.
 * @param[in]     const unsigned char *pkt - Buffer of transport packets, 1 or more.
 * @param[in]     int packetCount - Number of packets in the buffer.
 * @param[out]    int *complete - Result will contain 0 or 1. When 1, you are entitled to call _query_model()
 *                                to collect a fully formed PAT object, containing the entire PAT/PMT tree.
 * @return        Number of transport packets processed.
 */
size_t ltntstools_streammodel_write(void *hdl, const unsigned char *pkt, int packetCount, int *complete);

/**
 * @brief         Helper function. Print the entire current stream model to a file descriptor.
 *                Don't call this unless your previous _write call resulted in complete = 1.
 * @param[in]     void *hdl - Previously allocate context handle.
 * @param[in]     int fd - file descriptor
 */
void ltntstools_streammodel_dprintf(void *hdl, int fd);

/**
 * @brief         Collect a new allocation, containing a fully formed PAT/PMT tree.
 *                The caller is responsible for the object lifespan, free it once you're done.
 *                Don't call this function until your last _write call returns complete = 1.
 * @param[in]     void *hdl - Previously allocate context handle.
 * @param[in]     struct ltntstools_pat_s **pat - A full representation of the stream.
 * @return        0 - Success
 * @return      < 0 - Error
 */
int ltntstools_streammodel_query_model(void *hdl, struct ltntstools_pat_s **pat);

/**
 * @brief         Helper function.
 *                For a given pat object, typically returned from _querymodel, make a determination
 *                as to whether this represents a SPTS or MPTS stream.
 * @param[in]     void *hdl - Previously allocate context handle.
 * @param[in]     struct ltntstools_pat_s *pat - A full representation of the stream.
 * @return        0 - Success
 * @return      < 0 - Error
 */
int ltntstools_streammodel_is_model_mpts(void *hdl, struct ltntstools_pat_s *pat);

/**
 * @brief         Helper function.
 *                For a given pat object, typ[ically returned from _querymodel, find the first
 *                PMT and extract the PCR pid.
 * @param[in]     void *hdl - Previously allocate context handle.
 * @param[in]     struct ltntstools_pat_s *pat - A full representation of the stream.
 * @return        0 - Success
 * @return      < 0 - Error
 */
int ltntstools_streammodel_query_first_program_pcr_pid(void *hdl, struct ltntstools_pat_s *pat, uint16_t *PCRPID);


#define STREAMMODEL_CB_CRC_STATUS    1
#define             CRC_ARG_INVALID  0
#define             CRC_ARG_VALID    1

#define STREAMMODEL_CB_CONTEXT_PAT   1
#define STREAMMODEL_CB_CONTEXT_PMT   2
#define STREAMMODEL_CB_CONTEXT_CAT   3
#define STREAMMODEL_CB_CONTEXT_SDT   4
#define STREAMMODEL_CB_CONTEXT_BAT   5
#define STREAMMODEL_CB_CONTEXT_NIT   6
#define STREAMMODEL_CB_CONTEXT_TOT   7
#define STREAMMODEL_CB_CONTEXT_EIT   8

struct streammodel_callback_args_s
{
    uint32_t  status;    /* STREAMMODEL_CB_CRC_STATUS */
    uint32_t  context;   /* STREAMMODEL_CB_CONTEXT_PMT */
    uint32_t  arg;       /* CRC_ARG_VALID */
    void     *ptr;
};
typedef void (*ltntstools_streammodel_callback)(void *userContext, struct streammodel_callback_args_s *args);

/**
 * @brief         TR101290 helper function. Have the strem model check the
 *                PAT, PMT, NIT, BAT, SDT, EIT, TOT tables and ensure their CRCs are valid.
 *                Critical for a TR101290 P2.2 quality check.
 *                For a given pat object, typ[ically returned from _querymodel, find the first
 *                PMT and extract the PCR pid.
 * @param[in]     void *hdl - Previously allocate context handle.
 * @return        0 - Success, the request was activated.
 * @return      < 0 - Error
 */
int ltntstools_streammodel_enable_tr101290_section_checks(void *hdl, ltntstools_streammodel_callback cb);

#ifdef __cplusplus
};
#endif

#endif /* STREAMMODEL_H */
