/*
 * Copyright (c) 2016 Kernel Labs Inc. All Rights Reserved
 *
 * Address: Kernel Labs Inc., PO Box 745, St James, NY. 11780
 * Contact: sales@kernellabs.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tsudp_receiver_callback)(void *userContext, unsigned char *buf, int byteCount);
struct ltntstools_udp_receiver_s
{
	int skt;

	struct sockaddr_in sin;
	unsigned int ip_port;
	char ip_addr[32];
	int stripRTPHeader;

	unsigned char *rxbuffer;
	unsigned int rxbuffer_size;

	pthread_t threadId;
	int thread_running;
	int thread_terminate;
	int thread_complete;

	tsudp_receiver_callback cb;
	void *userContext;

	/* Debug dumping to disk */
	pthread_mutex_t fh_mutex;
	FILE *fh;
};

int ltntstools_udp_receiver_alloc(struct ltntstools_udp_receiver_s **p,
        unsigned int socket_buffer_size,
        const char *ip_addr,
        unsigned short ip_port,
        tsudp_receiver_callback cb,
        void *userContext,
	int stripRTPHeader);
void ltntstools_udp_receiver_free(struct ltntstools_udp_receiver_s **p);
ssize_t ltntstools_udp_receiver_read(struct ltntstools_udp_receiver_s *ctx, unsigned char *buf, unsigned int byteCount);
int ltntstools_udp_receiver_thread_start(struct ltntstools_udp_receiver_s *ctx);

/* Add or remove a specific network interface from the receiver, if its a multicast address */
int  ltntstools_udp_receiver_join_multicast(struct ltntstools_udp_receiver_s *p, char *ifname);
int  ltntstools_udp_receiver_drop_multicast(struct ltntstools_udp_receiver_s *p, char *ifname);

#ifdef __cplusplus
};
#endif
#endif /* UDP_RECEIVER_H */
