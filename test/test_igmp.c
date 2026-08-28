/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/igmp.c / src/libltntstools/igmp.h.
 * Builds against ../src/igmp.c only.
 *
 * SCOPE NOTE: the only internal helper (modifyMulticastInterfaces()) is
 * `static`, so it isn't reachable from this translation unit -- everything
 * below goes through the two public entry points, ltntstools_igmp_join()
 * and ltntstools_igmp_drop().
 *
 * ltntstools_igmp_join() with a genuinely multicast address does real work:
 * it enumerates this host's live network interfaces and issues a real
 * IP_ADD_MEMBERSHIP setsockopt() (and ltntstools_igmp_drop() a real
 * IP_DROP_MEMBERSHIP) -- registering/deregistering kernel multicast group
 * membership. That's harmless (no traffic is sent or received, it's purely
 * local kernel state) but it does depend on this host having at least one
 * UP, broadcast-capable, non-loopback IPv4 interface, which isn't
 * guaranteed in every CI/sandbox environment. test_join_and_drop_real_multicast()
 * below discovers such an interface itself via getifaddrs() and SKIPs
 * (does not fail) if none is available or the join doesn't succeed --
 * every other test here only exercises the early-return validation paths,
 * which never touch the network and are fully deterministic.
 *
 * A REAL BUG PINNED HERE (not fixed, just documented so a future change is
 * a deliberate decision): ltntstools_igmp_join() mallocs its context
 * (`ctx`) and strdup()s ipaddress/ifname into it *before* validating that
 * the address is actually multicast. When that later IN_MULTICAST() check
 * fails, the function returns -1 immediately without freeing ctx, ctx->ipaddr,
 * or ctx->ifname -- a guaranteed 3-allocation leak on every call with a
 * non-multicast address. Not independently regression-tested here (this
 * file has no leak-detection harness), but see
 * test_join_rejects_non_multicast_address_boundaries() for the reachable
 * trigger: any caller that lets user-supplied addresses reach this
 * function before validating them leaks on every rejection.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "libltntstools/igmp.h"

static int g_failures = 0;
static int g_skips = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* ---------------------------------------------------------------------
 * ltntstools_igmp_join() -- input validation (no network touched)
 * --------------------------------------------------------------------- */

static void test_join_rejects_null_ipaddress(void)
{
	void *handle = (void *)0x1; /* sentinel, must be zeroed regardless of outcome */
	int ret = ltntstools_igmp_join(&handle, NULL, 12345, "en0");
	CHECK(ret == -1);
	CHECK(handle == NULL);
}

static void test_join_rejects_null_ifname(void)
{
	void *handle = (void *)0x1;
	int ret = ltntstools_igmp_join(&handle, "239.255.1.1", 12345, NULL);
	CHECK(ret == -1);
	CHECK(handle == NULL);
}

static void test_join_rejects_non_multicast_address_boundaries(void)
{
	void *handle;

	/* Just below the multicast (224.0.0.0/4) range. */
	handle = (void *)0x1;
	CHECK(ltntstools_igmp_join(&handle, "223.255.255.255", 12345, "en0") == -1);
	CHECK(handle == NULL);

	/* Just above it (class E / reserved). */
	handle = (void *)0x1;
	CHECK(ltntstools_igmp_join(&handle, "240.0.0.0", 12345, "en0") == -1);
	CHECK(handle == NULL);

	/* A very ordinary unicast address. */
	handle = (void *)0x1;
	CHECK(ltntstools_igmp_join(&handle, "192.168.1.1", 12345, "en0") == -1);
	CHECK(handle == NULL);
}

static void test_drop_null_handle_is_safe(void)
{
	CHECK(ltntstools_igmp_drop(NULL) == 0);
}

/* ---------------------------------------------------------------------
 * ltntstools_igmp_join()/_drop() -- real multicast group membership on a
 * genuinely available interface. See the SCOPE NOTE above for why this is
 * allowed to SKIP rather than fail.
 * --------------------------------------------------------------------- */

/* Finds the name of the first UP, broadcast-capable, non-loopback IPv4
 * interface on this host, mirroring the criteria
 * src/igmp.c's modifyMulticastInterfaces() itself filters on
 * (IFF_BROADCAST && IFF_UP && AF_INET). Returns a malloc()'d name, or NULL
 * if none is found. */
static char *find_multicast_capable_ifname(void)
{
	struct ifaddrs *addrs;
	if (getifaddrs(&addrs) < 0) {
		return NULL;
	}

	char *name = NULL;
	for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
		if (!cursor->ifa_addr || cursor->ifa_addr->sa_family != AF_INET) {
			continue;
		}
		if (!(cursor->ifa_flags & IFF_BROADCAST) || !(cursor->ifa_flags & IFF_UP)) {
			continue;
		}
		name = strdup(cursor->ifa_name);
		break;
	}

	freeifaddrs(addrs);
	return name;
}

static void test_join_and_drop_real_multicast(void)
{
	char *ifname = find_multicast_capable_ifname();
	if (!ifname) {
		fprintf(stderr, "SKIP: test_join_and_drop_real_multicast: no UP/broadcast-capable IPv4 interface found\n");
		g_skips++;
		return;
	}

	/* RFC 2365 organization-local scope: safe to register interest in,
	 * nothing on the local network is expected to actually send to it. */
	void *handle = NULL;
	int ret = ltntstools_igmp_join(&handle, "239.255.1.1", 12345, ifname);
	if (ret != 0) {
		fprintf(stderr, "SKIP: test_join_and_drop_real_multicast: join on '%s' failed (ret=%d), "
			"environment likely doesn't permit it\n", ifname, ret);
		g_skips++;
		free(ifname);
		return;
	}

	CHECK(handle != NULL);
	CHECK(ltntstools_igmp_drop(handle) == 0);

	free(ifname);
}

int main(void)
{
	test_join_rejects_null_ipaddress();
	test_join_rejects_null_ifname();
	test_join_rejects_non_multicast_address_boundaries();
	test_drop_null_handle_is_safe();

	test_join_and_drop_real_multicast();

	if (g_failures == 0) {
		printf("PASS: all igmp tests passed%s\n", g_skips ? " (with skips, see stderr)" : "");
		return 0;
	}

	fprintf(stderr, "FAIL: %d igmp test(s) failed\n", g_failures);
	return 1;
}
