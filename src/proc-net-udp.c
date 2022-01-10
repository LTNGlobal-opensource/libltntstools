/*
 * Mechanism to find out which processes are suing which UDP sockets.
 *
 * Find all of the processes using sockets on the system,
 * open their socket statistics, make it available
 * in a clean struct. We use this to see which processes
 * are experiencing packet drops on the sockets.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

#include "libltntstools/proc-net-udp.h"

#define DEFAULT_REFRESH_MS 3000

#define LOCAL_DEBUG 0

struct ltntstools_proc_net_udp_ctx_s
{
    int refreshPeriodMS; /* How often (ms) does the framework updates its internal socket stats? */

  	pthread_mutex_t mutex;
	pthread_t threadId;
    int threadRunning, threadTerminate, threadTerminated;

    struct ltntstools_proc_net_udp_item_s *items;
    int itemCount;
};

int ltntstools_proc_net_udp_item_query(void *hdl, struct ltntstools_proc_net_udp_item_s **array, int *arrayCount)
{
#if LOCAL_DBEUG
    printf("%s()\n", __func__);
#endif
    struct ltntstools_proc_net_udp_ctx_s *ctx = (struct ltntstools_proc_net_udp_ctx_s *)hdl;

    int ret = -1; /* Assume failure */

    pthread_mutex_lock(&ctx->mutex);
    if (ctx->itemCount) {
        struct ltntstools_proc_net_udp_item_s *p = malloc(ctx->itemCount * sizeof(*p));
        memcpy(p, ctx->items, ctx->itemCount * sizeof(*p));
        *array = p;
        *arrayCount = ctx->itemCount;
        ret = 0; /* Success */
    }
    pthread_mutex_unlock(&ctx->mutex);

    return ret;
}

/* Lookup an unique entry in an array, based on slot number. */
struct ltntstools_proc_net_udp_item_s *ltntstools_proc_net_udp_find_inode(struct ltntstools_proc_net_udp_item_s *array, int arrayCount, uint64_t inode)
{
    if (array == NULL || arrayCount <= 0)
        return NULL;

    struct ltntstools_proc_net_udp_item_s *e;
    for (int i = 0; i < arrayCount; i++) {
        e = &array[i];
        if (e->inode == inode)
            return e;
    }

    return NULL;
}

void ltntstools_proc_net_udp_item_free(void *hdl, struct ltntstools_proc_net_udp_item_s *array)
{
    //struct ltntstools_proc_net_udp_ctx_s *ctx = (struct ltntstools_proc_net_udp_ctx_s *)hdl;
#if LOCAL_DBEUG
    printf("%s()\n", __func__);
#endif
    free(array);
}

void ltntstools_proc_net_udp_free(void *hdl)
{
#if LOCAL_DBEUG
    printf("%s()\n", __func__);
#endif
    struct ltntstools_proc_net_udp_ctx_s *ctx = (struct ltntstools_proc_net_udp_ctx_s *)hdl;

    if (ctx->threadRunning) {
        ctx->threadTerminate = 1;
        while (!ctx->threadTerminated) {
            usleep(10 * 1000);
        }
    }

    if (ctx->items) {
        ltntstools_proc_net_udp_item_free(ctx, ctx->items);
        ctx->items = NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

void ltntstools_proc_net_udp_item_dprintf(void *hdl, int fd, struct ltntstools_proc_net_udp_item_s *array, int arrayCount)
{
#if LOCAL_DBEUG
    printf("%s()\n", __func__);
#endif
    //struct ltntstools_proc_net_udp_ctx_s *ctx = (struct ltntstools_proc_net_udp_ctx_s *)hdl;

    dprintf(fd, "%8s %21s %21s %8s %8s %8s %s\n", "sl", "loc", "rem", "drops", "uid", "inode", "process");
    for (int i = 0; i < arrayCount; i++) {
        struct ltntstools_proc_net_udp_item_s *e = &array[i];
        dprintf(fd, "%8" PRIu64 " %21s %21s %8" PRIu64" %8" PRIu64 " %8" PRIu64, e->sl, e->locaddr, e->remaddr, e->drops, e->uid, e->inode);
        for (int j = 0; j < e->pidListCount; j++) {
            if (j == 0)
                dprintf(fd, " %" PRIu64 " %s", e->pidList[j].pid, e->pidList[j].comm);
            else
                dprintf(fd, ", %" PRIu64 " %s", e->pidList[j].pid, e->pidList[j].comm);
        }
        dprintf(fd, "\n");
    }
}

static struct ltntstools_proc_net_udp_item_s * _tableHelperItemFindiNode(uint64_t inode, struct ltntstools_proc_net_udp_item_s *items, int itemCount)
{
    for (int i = 0; i < itemCount; i++) {
        struct ltntstools_proc_net_udp_item_s *e = &items[i];

        if (inode == e->inode)
            return e;
    }

    return NULL;
}

static void _tableHelperItemAddProcess(struct ltntstools_proc_net_udp_item_s *item, uint64_t pid)
{
    if (item->pidListCount + 1 < LTNTSTOOLS_PROC_NET_UDP__MAX_PIDS) {
        item->pidList[item->pidListCount].pid = pid;

        /* Find the process by name */
        char fn[32];
        sprintf(fn, "/proc/%" PRIu64 "/comm", pid);
        FILE *fh = fopen(fn, "r");
        if (fh) {
            fgets((char *)&item->pidList[item->pidListCount].comm[0], sizeof(item->pidList[item->pidListCount].comm), fh);
            item->pidList[item->pidListCount].comm[ strlen((char *)item->pidList[item->pidListCount].comm) - 1] = 0;
            fclose(fh);
        }

        item->pidListCount++;
    }
}

/* For each entry in the UDP table, looking the process via inodes. Populate the remaining table data
 * with process ID's, inodes and process names.
 */
static int _tableBuilderProcesses(struct ltntstools_proc_net_udp_ctx_s *ctx, struct ltntstools_proc_net_udp_item_s *items, int itemCount)
{
    DIR *primaryDIR = opendir("/proc");
    if (!primaryDIR)
        return -1;

    struct dirent de;
    struct dirent *de_enum;
    int ret = 0;
    while (ret == 0) { /* For each item in the /proc dir */
        ret = readdir_r(primaryDIR, &de, &de_enum);
        if (!de_enum)
            break;

        if (!isdigit(de.d_name[0])) {
            /* Discard any entries that are not a process id */
            continue; 
        }

        /* Find the file descriptors associate with this /proc/<blah>/fd. */
        char fds[256];
        sprintf(&fds[0], "/proc/%s/fd", de.d_name);

        DIR *fdDIR = opendir(fds);
        if (!fdDIR)
            continue;

        struct dirent fd_de;
        struct dirent *fd_de_enum = NULL;
        ret = 0;
        while (ret == 0) { /* For each item in the /proc/PID/fd dir */
            ret = readdir_r(fdDIR, &fd_de, &fd_de_enum);
            if (!fd_de_enum)
                break;

            if (fd_de.d_type == DT_DIR) {
                continue;
            }

            char buf[256];
            char fqfn[64];
            sprintf(fqfn, "%s/%s", fds, fd_de.d_name);
            size_t len = readlink(fqfn, &buf[0], sizeof(buf));
            if (len <= 0) {
                continue; /* unable to determine (privs?) contents of link */
            }
            buf[len] = 0;

            if (strncmp("socket", &buf[0], 6) != 0)
                continue;

            /* convert symlink string 'socket:[4065562]' into the inode 4065562 */
            buf[len - 1] = 0; /* strip trailing ] */
            uint64_t inode = atoi(&buf[8]);
            struct ltntstools_proc_net_udp_item_s *item = _tableHelperItemFindiNode(inode, items, itemCount);
            if (!item)
                continue;

            /* Update the table, add a pid to a specific inode and stream. */
            _tableHelperItemAddProcess(item, atoi(de.d_name));
        }

	    closedir(fdDIR);
    }
    
    closedir(primaryDIR);

    return 0; /* Success */
}

void ltntstools_proc_net_udp_items_reset_drops(void *hdl)
{
    struct ltntstools_proc_net_udp_ctx_s *ctx = (struct ltntstools_proc_net_udp_ctx_s *)hdl;

    pthread_mutex_lock(&ctx->mutex);
    for (int i = 0; i < ctx->itemCount; i++) {
        struct ltntstools_proc_net_udp_item_s *e = &ctx->items[i];
        e->drops_reset = e->drops;
        e->drops_delta = 0;
    }
    pthread_mutex_unlock(&ctx->mutex);
}

/* Build a memory structure containing all of the records in /proc/net/udp */
static int _tableBuilderSockets(struct ltntstools_proc_net_udp_ctx_s *ctx)
{
#if LOCAL_DBEUG
    printf("%s()\n", __func__);
#endif

    /* Open /proc/net/udp and cache the results. */

    int itemCount = 0;
    struct ltntstools_proc_net_udp_item_s *items = NULL;

    FILE *fh = fopen("/proc/net/udp", "r");
    if (!fh) {
        return -1;
    }
    /*   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops             
        1203: 00000000:D459 00000000:0000 07 00000000:00000000 00:00000000 00000000    70        0 20892 2 0000000000000000 0         
    */
    char line[256];
    while (!feof(fh)) {
        memset(line, 0, sizeof(line));
        if (fgets(&line[0], sizeof(line), fh) == NULL) {
            break;
        }
        if (feof(fh))
            break;

        items = realloc(items, (itemCount + 1) * sizeof(*items));

        struct ltntstools_proc_net_udp_item_s *i = &items[itemCount++];
        i->pidListCount = 0;

        /* scanf a little buggy when parsing the line. Other tools (nettools), splits the
         * parsing into multiple stages for the local and remote addresses, so we'll do that too.
         * ..... after spending an hour trying to figure out what was wrong with sscanf.
         */
        char locaddr[64], remaddr[64];
        memset(locaddr, 0, sizeof(locaddr)); // TODO: Necessary?
        memset(remaddr, 0, sizeof(remaddr)); // TODO: Necessary?
        int fields = sscanf(line, "%ld: %64[0-9A-Fa-f]:%X %64[0-9A-Fa-f]:%X %*X %*x:%*x %*x:%*x %*x %" PRIu64 "%*d %" PRIu64 " %*d %*x %" PRIu64,
            &i->sl,
            &locaddr[0], (uint32_t *)&i->local_addr.sin_port,
            &remaddr[0], (uint32_t *)&i->remote_addr.sin_port,
            &i->uid, &i->inode, &i->drops);

        if (fields != 8) {
            itemCount--;
            continue; /* Error when we read the first line which doesn't match the scanf pattern */
        }

        sscanf(locaddr, "%X", &i->local_addr.sin_addr.s_addr);
        sscanf(remaddr, "%X", &i->remote_addr.sin_addr.s_addr);
        sprintf(i->locaddr, "%s:%d", inet_ntoa(i->local_addr.sin_addr), i->local_addr.sin_port);
        sprintf(i->remaddr, "%s:%d", inet_ntoa(i->remote_addr.sin_addr), i->remote_addr.sin_port);

        struct ltntstools_proc_net_udp_item_s *e = ltntstools_proc_net_udp_find_inode(ctx->items, ctx->itemCount, i->inode);
        if (e) {
            /* Find a preview record for this, compare the drops and flag a change if needed. */
            if (e->drops != i->drops) {
                i->drops_delta = i->drops - i->drops_reset;
                i->drops_delta = i->drops - i->drops_reset;
            } else {
                i->drops_reset = e->drops_reset;
                i->drops_delta = i->drops - i->drops_reset;
            }
        } else {
            /* First time we've see this socket, remember the startup drops */
            i->drops_reset = i->drops;
            i->drops_delta = 0;
        }

    }
    fclose(fh);

    /* Now we have to go and find all the processes by connecting the inode entries in the
     * table, via process file decriptors, to the process themselves. We'll collect
     * inodes, PIDS and process names along the way.
     */
    _tableBuilderProcesses(ctx, items, itemCount);

    /* Update the instance context, so we can use it later when the caller wans a query. */
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->items) {
        ltntstools_proc_net_udp_item_free(ctx, ctx->items);
        ctx->itemCount = 0;
    }
    ctx->items = items;
    ctx->itemCount = itemCount;
    pthread_mutex_unlock(&ctx->mutex);

    return 0; /* Success */
}

static void *proc_net_udp_threadfunc(void *p)
{
#if LOCAL_DBEUG
    printf("%s()\n", __func__);
#endif
    struct ltntstools_proc_net_udp_ctx_s *ctx = (struct ltntstools_proc_net_udp_ctx_s *)p;

    pthread_detach(ctx->threadId);

    /* Give the User a fast rapid closedown mechanism */
    int queryPeriodMs = 0;
	ctx->threadRunning = 1;
	while (!ctx->threadTerminate) {
        usleep(20 * 1000);
        queryPeriodMs += 20;
        if (queryPeriodMs >= ctx->refreshPeriodMS) {
            queryPeriodMs = 0;
            _tableBuilderSockets(ctx);
        }
    }

	ctx->threadTerminated= 1;
	ctx->threadRunning = 0;
	pthread_exit(0);
}

int ltntstools_proc_net_udp_alloc(void **hdl)
{
#if LOCAL_DBEUG
    printf("%s()\n", __func__);
#endif
    struct ltntstools_proc_net_udp_ctx_s *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    ctx->refreshPeriodMS = DEFAULT_REFRESH_MS;
    pthread_mutex_init(&ctx->mutex, NULL);

    pthread_create(&ctx->threadId, NULL, proc_net_udp_threadfunc, ctx);

    *hdl = ctx;

    return 0; /* Success */
}
