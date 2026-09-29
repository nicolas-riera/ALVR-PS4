#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <orbis/libkernel.h>

static int g_sock = -1;
static sockaddr_in g_dest;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_ring[LOG_RING_LINES][LOG_LINE_MAX];
static int g_ring_head = 0;
static int g_ring_count = 0;
static unsigned g_seq = 0;

void log_init()
{
#if !ALVR_PS4_DEV
    return; // UDP logs only in the Dev build (make VARIANT=dev)
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock >= 0) {
        int on = 1;
        setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }
    memset(&g_dest, 0, sizeof(g_dest));
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons(LOG_UDP_PORT);
    g_dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
}

void log_printf(const char *fmt, ...)
{
    char body[LOG_LINE_MAX - 16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);

    uint64_t ms = sceKernelGetProcessTime() / 1000;
    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line), "[%6llu.%03llu] %s",
             (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000), body);

    printf("%s\n", line);

    if (g_sock >= 0) {
        // Sequence number lets the receiver detect dropped packets.
        char pkt[LOG_LINE_MAX + 16];
        int n = snprintf(pkt, sizeof(pkt), "%u|%s", g_seq, line);
        sendto(g_sock, pkt, n, 0, (sockaddr *)&g_dest, sizeof(g_dest));
    }
    g_seq++;

    strncpy(g_ring[g_ring_head], line, LOG_LINE_MAX - 1);
    g_ring[g_ring_head][LOG_LINE_MAX - 1] = 0;
    g_ring_head = (g_ring_head + 1) % LOG_RING_LINES;
    if (g_ring_count < LOG_RING_LINES)
        g_ring_count++;

    pthread_mutex_unlock(&g_lock);
}

int log_snapshot(char out[LOG_RING_LINES][LOG_LINE_MAX])
{
    pthread_mutex_lock(&g_lock);
    int start = (g_ring_head - g_ring_count + LOG_RING_LINES) % LOG_RING_LINES;
    for (int i = 0; i < g_ring_count; i++)
        memcpy(out[i], g_ring[(start + i) % LOG_RING_LINES], LOG_LINE_MAX);
    int n = g_ring_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}
