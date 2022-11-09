
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netdb.h>
#include "libltntstools/igmp.h"

struct ltntstools_igp_ctx_s
{
	int skt;
	struct sockaddr_in sin;
	char *ipaddr;
	char *ifname;
};

/* IGMP Handling */
/*
 ipaddr = a string to print, eg 227.1.1.1:4001
 option IP_ADD_MEMBERSHIP or IP_DROP_MEMBERSHIP
 ifname. Eg eno2
 */
static int modifyMulticastInterfaces(struct ltntstools_igp_ctx_s *ctx, int skt, struct sockaddr_in *sin, int option)
{
	int didModify = 0;

	/* Setup multicast on all IPV4 network interfaces, IPV6 interfaces are ignored */
	struct ifaddrs *addrs;
	int result = getifaddrs(&addrs);
	if (result >= 0) {
		const struct ifaddrs *cursor = addrs;
		while (cursor != NULL) {
			if ((cursor->ifa_flags & IFF_BROADCAST) && (cursor->ifa_flags & IFF_UP) &&
				(cursor->ifa_addr->sa_family == AF_INET)) {

				char host[NI_MAXHOST];

				int r = getnameinfo(cursor->ifa_addr,
					cursor->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
					host, NI_MAXHOST,
					NULL, 0, NI_NUMERICHOST);
				if (r == 0) {
#if 0
					printf("ifa_name:%s flags:%x host:%s\n", cursor->ifa_name, cursor->ifa_flags, host);
#endif
					int shouldModify = 1;

					if (ctx->ifname && (strcasecmp(ctx->ifname, cursor->ifa_name) != 0))
						shouldModify = 0;

					if (shouldModify) {

						/* Enable multicast reception on this specific interface */
						/* join multicast group */
						struct ip_mreq mreq;
						mreq.imr_multiaddr.s_addr = sin->sin_addr.s_addr;
						mreq.imr_interface.s_addr = ((struct sockaddr_in *)cursor->ifa_addr)->sin_addr.s_addr;
						if (setsockopt(skt, IPPROTO_IP, option, (void *)&mreq, sizeof(mreq)) < 0) {
							fprintf(stderr, "%s() cannot %s multicast group %s on iface %s\n", __func__, ctx->ipaddr,
								option == IP_ADD_MEMBERSHIP ? "join" : "leave",
								cursor->ifa_name
								);
							return -1;
						} else {
							didModify++;
							fprintf(stderr, "%s() %s multicast group %s ok on iface %s\n", __func__, ctx->ipaddr,
								option == IP_ADD_MEMBERSHIP ? "join" : "leave",
								cursor->ifa_name
								);
						}
					}
				}
			}
			cursor = cursor->ifa_next;
		}
	}

	freeifaddrs(addrs);

	if (didModify)
		return 0; /* Success */
	else
		return -1;
}

/* Eg. 227.1.1.1, 1234, eno2 */
int ltntstools_igmp_join(void **handle, const char *ipaddress, uint16_t port, char *ifname)
{
	*handle = 0;
	
	if ((!ifname) || (!ipaddress))
		return -1;

	struct ltntstools_igp_ctx_s *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->ipaddr = strdup(ipaddress);
	ctx->ifname = strdup(ifname);
	ctx->sin.sin_family = AF_INET;
	ctx->sin.sin_port = htons(port);
	ctx->sin.sin_addr.s_addr = inet_addr(ipaddress);

	if (!IN_MULTICAST(ntohl(ctx->sin.sin_addr.s_addr)))
		return -1;

	/* Allocate a socket and retain it. */
	ctx->skt = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx->skt < 0) {
		free(ctx->ipaddr);
		free(ctx->ifname);
		free(ctx);
		return -1;
	}

	int ret = modifyMulticastInterfaces(ctx, ctx->skt, &ctx->sin, IP_ADD_MEMBERSHIP);
	if (ret != 0) {
		fprintf(stderr, "%s() unable to leave multicastgroup\n", __func__);
		free(ctx->ipaddr);
		free(ctx->ifname);
		free(ctx);
		return -1;
	}
	*handle = ctx;

	return 0; /* Success */
}

int ltntstools_igmp_drop(void *handle)
{
	struct ltntstools_igp_ctx_s *ctx = (struct ltntstools_igp_ctx_s *)handle;
	if (!ctx)
		return 0;

	int ret = modifyMulticastInterfaces(ctx, ctx->skt, &ctx->sin, IP_DROP_MEMBERSHIP);
	if (ret != 0) {
		fprintf(stderr, "%s() unable to leave multicastgroup, ignoring.\n", __func__);
	}
	free(ctx->ipaddr);
	free(ctx->ifname);
	free(ctx);

	return 0; /* Success */
}
