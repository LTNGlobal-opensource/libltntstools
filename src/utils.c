
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netdb.h>

int ltnpthread_setname_np(pthread_t thread, const char *name)
{
#if defined(__linux__)
extern int pthread_setname_np(pthread_t thread, const char *name);
	return pthread_setname_np(thread, name);
#endif
#if defined(__APPLE__)
	/* We don't support thread naming on OSX, yet. */
	return 0;
#endif
}

#if 0
/* In string 'str', find occurences of character src and replace with character dst. */
/* return the number of substituions occured. */
int character_replace(char *str, char src, char dst)
{
	int c = 0;

	for (int i = 0; i < strlen(str); i++) {
		if (str[i] == src) {
			str[i] = dst;
			c++;
		}
	}

	return c;
}

int networkInterfaceExists(const char *ifname)
{
	int exists = 0;

	/* Setup multicast on all IPV4 network interfaces, IPV6 interfaces are ignored */
	struct ifaddrs *addrs;
	int result = getifaddrs(&addrs);
	if (result >= 0) {
		const struct ifaddrs *cursor = addrs;
		while (cursor != NULL) {
#if 0
			char host[NI_MAXHOST];
			int r = getnameinfo(cursor->ifa_addr,
				cursor->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
				host, NI_MAXHOST,
				NULL, 0, NI_NUMERICHOST);
			printf("name: %s\n", cursor->ifa_name);
			printf("\t host: %s\n", host);
			if (cursor->ifa_flags & IFF_BROADCAST)
				printf("\tflags: IFF_BROADCAST = true\n");
			else
				printf("\tflags: IFF_BROADCAST = false\n");
			if (cursor->ifa_flags & IFF_UP)
				printf("\tflags: IFF_UP = true\n");
			else
				printf("\tflags: IFF_UP = false\n");
#endif
			if (/* (cursor->ifa_flags & IFF_BROADCAST) && */ (cursor->ifa_flags & IFF_UP) &&
				(cursor->ifa_addr) && 
				(cursor->ifa_addr->sa_family == AF_INET)) {

				char host[NI_MAXHOST];

				int r = getnameinfo(cursor->ifa_addr,
					cursor->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
					host, NI_MAXHOST,
					NULL, 0, NI_NUMERICHOST);
				if (r == 0) {
					if (strcmp(cursor->ifa_name, ifname) == 0) {
						exists = 1;
						break;
					}
				}
			}
			cursor = cursor->ifa_next;
		}
	}

	freeifaddrs(addrs);

	return exists;
}

void networkInterfaceList()
{
	/* Setup multicast on all IPV4 network interfaces, IPV6 interfaces are ignored */
	struct ifaddrs *addrs;
	int result = getifaddrs(&addrs);
	if (result >= 0) {
		const struct ifaddrs *cursor = addrs;
		while (cursor != NULL) {
			if (/* (cursor->ifa_flags & IFF_BROADCAST) && */ (cursor->ifa_flags & IFF_UP) &&
				(cursor->ifa_addr) &&
				(cursor->ifa_addr->sa_family == AF_INET)) {

				char host[NI_MAXHOST];

				int r = getnameinfo(cursor->ifa_addr,
					cursor->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
					host, NI_MAXHOST,
					NULL, 0, NI_NUMERICHOST);
				if (r == 0) {
					printf("\t%s : %s\n", cursor->ifa_name, host);
				}
			}
			cursor = cursor->ifa_next;
		}
	}

	freeifaddrs(addrs);
}

int network_addr_compare(
	struct iphdr *src_iphdr, struct udphdr *src_udphdr,
	struct iphdr *dst_iphdr, struct udphdr *dst_udphdr)
{
#ifdef __APPLE__
	if (src_iphdr->ip_src.s_addr != dst_iphdr->ip_src.s_addr)
		return 0;
	if (src_iphdr->ip_dst.s_addr != dst_iphdr->ip_dst.s_addr)
		return 0;
#endif
#ifdef __linux__
	if (src_iphdr->saddr != dst_iphdr->saddr)
		return 0;
	if (src_iphdr->daddr != dst_iphdr->daddr)
		return 0;
#endif

#ifdef __APPLE__
	uint16_t src_port_source = ntohs(*((uint16_t *)src_udphdr));
	uint16_t dst_port_source = ntohs(*((uint16_t *)dst_udphdr));
	uint16_t src_port_dest = ntohs(*((uint16_t *)src_udphdr + 1));
	uint16_t dst_port_dest = ntohs(*((uint16_t *)dst_udphdr + 1));
#else
	uint16_t src_port_source = ntohs(src_udphdr->source);
	uint16_t dst_port_source = ntohs(dst_udphdr->source);
	uint16_t dst_port_dest = ntohs(dst_udphdr->dest);
	uint16_t src_port_dest = ntohs(src_udphdr->dest);
#endif

	if (src_port_source != dst_port_source)
		return 0;
	if (src_port_dest != dst_port_dest)
		return 0;

	return 1; /* Success, matched */
}

char *network_stream_ascii(struct iphdr *iphdr, struct udphdr *udphdr)
{
	struct in_addr dstaddr, srcaddr;
#ifdef __linux__
	srcaddr.s_addr = iphdr->saddr;
	dstaddr.s_addr = iphdr->daddr;
#endif
#ifdef __APPLE__
	srcaddr.s_addr = iphdr->ip_src.s_addr;
	dstaddr.s_addr = iphdr->ip_dst.s_addr;
#endif

#ifdef __APPLE__
	uint16_t src_port = ntohs(*((uint16_t *)udphdr));
	uint16_t dst_port = ntohs(*((uint16_t *)udphdr + 1));
#else
	uint16_t src_port = ntohs(udphdr->source);
	uint16_t dst_port = ntohs(udphdr->dest);
#endif

	char *str = malloc(256);
	sprintf(str, "%s:%d", inet_ntoa(srcaddr), src_port);
	sprintf(str + strlen(str), " -> %s:%d", inet_ntoa(dstaddr), dst_port);

	return str;
}
