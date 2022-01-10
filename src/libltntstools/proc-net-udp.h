/**
 * @file        proc-net-udp.h
 * @author      Steven Toth <steven.toth@ltnglobal.com>
 * @copyright   Copyright (c) 2021 LTN Global,Inc. All Rights Reserved.
 * @brief       Query all running processes. See which processes have udp sockets open.
 *              Return their state along with any packet loss.
 */

#ifndef _PROC_NET_UDP_H
#define _PROC_NET_UDP_H

#include <time.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define LTNTSTOOLS_PROC_NET_UDP__MAX_PIDS 4

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       Allocate a new probe, for use with all other calls.
 * @param[out]  void **handle - returned object.
 * @return      0 - Success
 * @return      < 0 - Error
 */
int  ltntstools_proc_net_udp_alloc(void **hdl);

/**
 * @brief       Free a previously allocated probe.
 * @param[in]   void *handle - ltntstools_probe_ltnencoder_alloc()
 */
void ltntstools_proc_net_udp_free(void *hdl);

struct ltntstools_proc_net_udp_item_s
{
    /* Absolutely no pointers or allocations here, must be static.
     * caller is expected to receive a single allocation containing multiple
     * instances of this struct, and be able to free the single allocation to
     * destroy all instances. (via ltntstools_proc_net_udp_item_free())
     */

    uint64_t pid; /* process identifier */

    /* From /proc/pid/net/udp */
    uint64_t inode;
    uint64_t uid;
    uint64_t sl;
    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;
    uint64_t drops;
    uint64_t drops_reset;
    uint64_t drops_delta;   /* Number of drops between drops_reset and now */

    char locaddr[64]; /* Eg. 0.0.0.0:4001 */
    char remaddr[64]; /* Eg. 0.0.0.0:4001 */

    /* Processes linked to this socket */
    struct {
        uint64_t pid;
        unsigned char comm[64];
    } pidList[LTNTSTOOLS_PROC_NET_UDP__MAX_PIDS];
    int pidListCount;
};

/**
 * @brief       Query all known sockets and return in an array.
 *              User responsible for array destruction.
 * @param[in]   void *handle - Context returned from ltntstools_proc_net_udp_alloc()
 * @return      0 - Success
 * @return      < 0 - Error, undisclosed issue.
 */
int  ltntstools_proc_net_udp_item_query(void *hdl, struct ltntstools_proc_net_udp_item_s **array, int *arrayCount);

void ltntstools_proc_net_udp_item_free(void *hdl, struct ltntstools_proc_net_udp_item_s *array);

void ltntstools_proc_net_udp_items_reset_drops(void *hdl);

void ltntstools_proc_net_udp_item_dprintf(void *hdl, int fd, struct ltntstools_proc_net_udp_item_s *array, int arrayCount);

struct ltntstools_proc_net_udp_item_s *ltntstools_proc_net_udp_find_inode(struct ltntstools_proc_net_udp_item_s *array, int arrayCount, uint64_t inode);

#ifdef __cplusplus
};
#endif

#endif /* _PROC_NET_UDP_H */
