/* Copyright LTN Global Communications, Inc. All Rights Reserved. */

/* Unit tests for src/udp_receiver.c / src/libltntstools/udp_receiver.h.
 * Self-contained: only libc/BSD sockets + pthread, no other .c dependency.
 * Links -lpthread.
 *
 * struct ltntstools_udp_receiver_s is fully exposed in the public header
 * (not opaque), so these tests use ctx->skt directly with getsockname() to
 * discover which ephemeral port the OS actually bound (every test below
 * requests port 0), avoiding "port already in use" flakiness entirely.
 * The end-to-end receive tests use real loopback (127.0.0.1) UDP sockets
 * and poll (with generous timeouts) for the background thread's callback
 * to fire, since this module's whole design defers delivery to a
 * background thread reading from a real socket.
 *
 * THREE REAL bugs were found while writing these tests and have been FIXED
 * in src/udp_receiver.c:
 *
 * 1. (Confirmed via a file-descriptor-count repro) ltntstools_udp_receiver_alloc()
 *    leaked the just-created socket file descriptor on every failure path
 *    after socket() succeeded (SO_RCVBUF, SO_REUSEADDR, bind(), fcntl(),
 *    or the rxbuffer malloc() failing) -- each of those did `free(ctx);
 *    return -1;` without ever close()ing ctx->skt. Confirmed via repro:
 *    binding to 203.0.113.1 (an RFC 5737 TEST-NET-3 address guaranteed not
 *    to be assigned to any local interface, so bind() reliably fails)
 *    left one extra open fd behind before the fix; zero extra after.
 *    test_alloc_rejects_invalid_bind_address_and_does_not_leak_fd() below
 *    is the regression test. bind() is the most realistic/reachable of
 *    these paths (a caller retrying a failed bind, e.g. "address already
 *    in use" or a misconfigured interface address, would exhaust the
 *    process's fd table over time); the other three paths got the same
 *    fix for the same reason, though they're harder to trigger
 *    deterministically in a test (SO_RCVBUF/SO_REUSEADDR essentially never
 *    fail in practice, and malloc() failure requires OOM).
 *
 * 2. (Confirmed via a standalone repro) In udp_receiver_threadfunc()'s
 *    stripRTPHeader branch, `int bytes = ((rxbytes - 12) / 188) * 188;`
 *    computed on `rxbytes` (a size_t, i.e. unsigned) -- for any received
 *    UDP datagram shorter than the 12-byte RTP header being stripped,
 *    `rxbytes - 12` underflowed to a huge unsigned value, which then
 *    narrowed into the signed `int bytes` as garbage (confirmed via repro:
 *    a 5-byte datagram produced bytes == -72). That garbage/negative byte
 *    count was then handed directly to the caller's callback as `int
 *    byteCount` -- undefined/dangerous for any reasonable callback
 *    implementation (e.g. anything that later treats byteCount as
 *    unsigned, like memcpy()'s size_t parameter, would see it as a huge
 *    positive value again). Reachable by any malformed/truncated UDP
 *    datagram arriving on a stripRTPHeader=1 receiver -- entirely
 *    plausible on a real network, especially from untrusted
 *    broadcast/multicast sources. Fixed by requiring rxbytes >= 12 before
 *    doing the subtraction; shorter datagrams are now silently dropped
 *    (no callback) rather than misinterpreted.
 *    test_receive_short_datagram_with_rtp_strip_does_not_underflow() below
 *    is the regression test.
 *
 * 3. The same use-after-free race already found and fixed in
 *    smoother-pcr.c, probes.c, smoother-rtp.c and segmentwriter.c:
 *    ltntstools_udp_receiver_thread_start() spawned the background thread
 *    via a bare pthread_create() and returned its result directly, while
 *    thread_running was only ever set by the thread itself (inside
 *    udp_receiver_threadfunc()) after it started running -- not by the
 *    caller, synchronously, before/at spawn time.
 *    ltntstools_udp_receiver_free() gates its "wait for the thread to
 *    terminate" logic on that flag, so free() called immediately after
 *    thread_start() (no delay) could skip the wait entirely and let the
 *    thread go on to use ctx->skt/ctx->rxbuffer after free() had already
 *    closed/freed them. This module differs slightly from the other four:
 *    the thread isn't auto-started by alloc(), so the race only exists for
 *    callers who do call thread_start(). Fixed the same way: set
 *    thread_running = 1 in the parent, synchronously, immediately after a
 *    successful pthread_create() (not unconditionally before it, which
 *    would instead hang free() forever if pthread_create() itself ever
 *    failed). test_thread_start_and_free_basic() below stress-loops
 *    thread_start()+free() with no delay between them, exactly the
 *    pattern that used to race.
 *
 * ALSO NOTED BUT NOT FIXED: ltntstools_udp_receiver_read() is declared in
 * udp_receiver.h but has no implementation anywhere in this codebase --
 * calling it would fail to link. Not tested here (nothing to call); not
 * fixed, since inventing an implementation would be guessing at intended
 * semantics rather than fixing a defect in existing behavior.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "libltntstools/udp_receiver.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++; \
		} \
	} while (0)

/* -------- helpers -------- */

static unsigned short get_bound_port(struct ltntstools_udp_receiver_s *ctx)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	if (getsockname(ctx->skt, (struct sockaddr *)&addr, &len) != 0)
		return 0;
	return ntohs(addr.sin_port);
}

static int count_open_fds(void)
{
	int count = 0;
	for (int fd = 0; fd < 256; fd++) {
		if (fcntl(fd, F_GETFD) >= 0)
			count++;
	}
	return count;
}

static void send_udp(unsigned short port, const void *buf, size_t len)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	assert(s >= 0);

	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(port);
	dst.sin_addr.s_addr = inet_addr("127.0.0.1");

	sendto(s, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
	close(s);
}

/* -------- callback capture (invoked from the receiver's own thread) -------- */

#define MAX_CAPTURE 2048
struct captured_s
{
	int byteCount; /* raw, unclamped -- may be negative if a regression reappears */
	unsigned char buf[MAX_CAPTURE];
	int copiedBytes; /* how much of buf is actually valid */
};

static pthread_mutex_t g_captureMutex = PTHREAD_MUTEX_INITIALIZER;
static int g_captureCount;
static struct captured_s g_captures[8];

static void reset_capture(void)
{
	pthread_mutex_lock(&g_captureMutex);
	g_captureCount = 0;
	pthread_mutex_unlock(&g_captureMutex);
}

static int get_capture_count(void)
{
	int n;
	pthread_mutex_lock(&g_captureMutex);
	n = g_captureCount;
	pthread_mutex_unlock(&g_captureMutex);
	return n;
}

static void capture_cb(void *userContext, unsigned char *buf, int byteCount)
{
	pthread_mutex_lock(&g_captureMutex);
	if (g_captureCount < (int)(sizeof(g_captures) / sizeof(g_captures[0]))) {
		struct captured_s *c = &g_captures[g_captureCount];
		c->byteCount = byteCount;
		/* Defensive clamp here is test-harness safety, not a workaround
		 * for the library: if byteCount were ever garbage again, this
		 * avoids an OOB memcpy crashing the test binary itself, while
		 * c->byteCount above still preserves the raw (possibly bogus)
		 * value for assertions to catch. */
		c->copiedBytes = (byteCount > 0 && byteCount <= MAX_CAPTURE) ? byteCount : 0;
		if (c->copiedBytes > 0)
			memcpy(c->buf, buf, c->copiedBytes);
		g_captureCount++;
	}
	pthread_mutex_unlock(&g_captureMutex);
}

static int wait_for_captures(int n, int timeoutMs)
{
	struct timeval start, now;
	gettimeofday(&start, NULL);
	while (1) {
		if (get_capture_count() >= n)
			return 1;
		gettimeofday(&now, NULL);
		int64_t elapsedMs = (now.tv_sec - start.tv_sec) * 1000LL + (now.tv_usec - start.tv_usec) / 1000LL;
		if (elapsedMs >= timeoutMs)
			return 0;
		usleep(2 * 1000);
	}
}

/* -------- alloc / lifecycle -------- */

static void test_alloc_rejects_null_ip_addr(void)
{
	struct ltntstools_udp_receiver_s *p = NULL;
	CHECK(ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, NULL, 0, capture_cb, NULL, 0) == -1);
}

static void test_alloc_and_free_basic(void)
{
	struct ltntstools_udp_receiver_s *p = NULL;
	CHECK(ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, "127.0.0.1", 0, capture_cb, NULL, 0) == 0);
	CHECK(p != NULL);

	ltntstools_udp_receiver_free(&p);
	CHECK(p == NULL); /* *p is zeroed by free() */
}

/* Regression test for the fixed fd-leak bug: see file header. */
static void test_alloc_rejects_invalid_bind_address_and_does_not_leak_fd(void)
{
	int before = count_open_fds();

	struct ltntstools_udp_receiver_s *p = NULL;
	/* 203.0.113.1 is RFC 5737 TEST-NET-3, guaranteed not assigned to any
	 * local interface -> bind() reliably fails. */
	int ret = ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, "203.0.113.1", 12345, capture_cb, NULL, 0);
	CHECK(ret == -1);
	CHECK(p == NULL);

	int after = count_open_fds();
	CHECK(after == before);
}

/* -------- multicast -------- */

static void test_join_multicast_rejects_non_multicast_address(void)
{
	struct ltntstools_udp_receiver_s *p = NULL;
	ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, "127.0.0.1", 0, capture_cb, NULL, 0);

	CHECK(ltntstools_udp_receiver_join_multicast(p, "lo0") == -1);
	CHECK(ltntstools_udp_receiver_drop_multicast(p, "lo0") == -1);

	ltntstools_udp_receiver_free(&p);
}

/* -------- thread lifecycle / race regression -------- */

static void test_thread_start_and_free_basic(void)
{
	/* Stress the thread_start()-immediately-followed-by-free() pattern
	 * that used to race (see file header). */
	for (int i = 0; i < 20; i++) {
		struct ltntstools_udp_receiver_s *p = NULL;
		CHECK(ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, "127.0.0.1", 0, capture_cb, NULL, 0) == 0);
		CHECK(ltntstools_udp_receiver_thread_start(p) == 0);
		ltntstools_udp_receiver_free(&p);
	}
}

/* -------- end-to-end: real loopback UDP receive -------- */

static void test_receive_delivers_data_without_rtp_strip(void)
{
	struct ltntstools_udp_receiver_s *p = NULL;
	CHECK(ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, "127.0.0.1", 0, capture_cb, NULL, 0 /* no strip */) == 0);
	unsigned short port = get_bound_port(p);
	CHECK(port != 0);
	CHECK(ltntstools_udp_receiver_thread_start(p) == 0);
	reset_capture();

	unsigned char payload[20];
	for (int i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (unsigned char)i;
	send_udp(port, payload, sizeof(payload));

	CHECK(wait_for_captures(1, 2000));
	CHECK(get_capture_count() == 1);

	pthread_mutex_lock(&g_captureMutex);
	struct captured_s c = g_captures[0];
	pthread_mutex_unlock(&g_captureMutex);

	CHECK(c.byteCount == (int)sizeof(payload));
	CHECK(c.copiedBytes == (int)sizeof(payload));
	CHECK(memcmp(c.buf, payload, sizeof(payload)) == 0);

	ltntstools_udp_receiver_free(&p);
}

static void test_receive_strips_rtp_header_when_enabled(void)
{
	struct ltntstools_udp_receiver_s *p = NULL;
	CHECK(ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, "127.0.0.1", 0, capture_cb, NULL, 1 /* strip */) == 0);
	unsigned short port = get_bound_port(p);
	CHECK(port != 0);
	CHECK(ltntstools_udp_receiver_thread_start(p) == 0);
	reset_capture();

	unsigned char rtpHeader[12];
	memset(rtpHeader, 0xAA, sizeof(rtpHeader)); /* content irrelevant, only its length matters */

	unsigned char tsPayload[2 * 188];
	for (int i = 0; i < (int)sizeof(tsPayload); i++)
		tsPayload[i] = (unsigned char)(i & 0xff);

	unsigned char datagram[sizeof(rtpHeader) + sizeof(tsPayload)];
	memcpy(datagram, rtpHeader, sizeof(rtpHeader));
	memcpy(datagram + sizeof(rtpHeader), tsPayload, sizeof(tsPayload));
	send_udp(port, datagram, sizeof(datagram));

	CHECK(wait_for_captures(1, 2000));
	CHECK(get_capture_count() == 1);

	pthread_mutex_lock(&g_captureMutex);
	struct captured_s c = g_captures[0];
	pthread_mutex_unlock(&g_captureMutex);

	CHECK(c.byteCount == (int)sizeof(tsPayload));
	CHECK(c.copiedBytes == (int)sizeof(tsPayload));
	CHECK(memcmp(c.buf, tsPayload, sizeof(tsPayload)) == 0); /* RTP header stripped */

	ltntstools_udp_receiver_free(&p);
}

/* Regression test for the fixed unsigned-underflow bug: see file header. */
static void test_receive_short_datagram_with_rtp_strip_does_not_underflow(void)
{
	struct ltntstools_udp_receiver_s *p = NULL;
	CHECK(ltntstools_udp_receiver_alloc(&p, 2 * 1024 * 1024, "127.0.0.1", 0, capture_cb, NULL, 1 /* strip */) == 0);
	unsigned short port = get_bound_port(p);
	CHECK(port != 0);
	CHECK(ltntstools_udp_receiver_thread_start(p) == 0);
	reset_capture();

	/* Shorter than the 12-byte RTP header being stripped. */
	unsigned char shortDatagram[5] = { 1, 2, 3, 4, 5 };
	send_udp(port, shortDatagram, sizeof(shortDatagram));

	/* Follow up with a normal, well-formed datagram on the same socket so
	 * we have a positive, bounded signal that the receive loop is still
	 * alive and processing (rather than just waiting out a timeout). */
	unsigned char rtpHeader[12];
	memset(rtpHeader, 0, sizeof(rtpHeader));
	unsigned char tsPayload[188];
	memset(tsPayload, 0x47, sizeof(tsPayload));
	unsigned char goodDatagram[sizeof(rtpHeader) + sizeof(tsPayload)];
	memcpy(goodDatagram, rtpHeader, sizeof(rtpHeader));
	memcpy(goodDatagram + sizeof(rtpHeader), tsPayload, sizeof(tsPayload));
	send_udp(port, goodDatagram, sizeof(goodDatagram));

	CHECK(wait_for_captures(1, 2000));

	pthread_mutex_lock(&g_captureMutex);
	int n = g_captureCount;
	struct captured_s captures[8];
	memcpy(captures, g_captures, sizeof(captures));
	pthread_mutex_unlock(&g_captureMutex);

	/* The short datagram must never have produced a callback at all (no
	 * garbage/negative byteCount); only the well-formed one should have. */
	CHECK(n == 1);
	if (n >= 1) {
		CHECK(captures[0].byteCount == (int)sizeof(tsPayload));
		CHECK(captures[0].byteCount >= 0); /* would be -72 before the fix */
	}

	ltntstools_udp_receiver_free(&p);
}

int main(void)
{
	test_alloc_rejects_null_ip_addr();
	test_alloc_and_free_basic();
	test_alloc_rejects_invalid_bind_address_and_does_not_leak_fd();

	test_join_multicast_rejects_non_multicast_address();

	test_thread_start_and_free_basic();

	test_receive_delivers_data_without_rtp_strip();
	test_receive_strips_rtp_header_when_enabled();
	test_receive_short_datagram_with_rtp_strip_does_not_underflow();

	if (g_failures == 0) {
		printf("PASS: all udp_receiver tests passed\n");
		return 0;
	}

	fprintf(stderr, "FAIL: %d udp_receiver test(s) failed\n", g_failures);
	return 1;
}
