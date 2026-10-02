#include "trackview.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "trackview_proto.h"

#define TRACKVIEW_MAX_VIEWERS 4

struct Viewer {
    sockaddr_in addr;
    uint64_t last_hello_us;
};

static int g_sock = -1;
static Viewer g_viewers[TRACKVIEW_MAX_VIEWERS];
static int g_viewer_count;
static uint64_t g_last_send_us;
static uint32_t g_seq;

void trackview_init()
{
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        LOG("trackview: socket failed");
        return;
    }
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(TRACKVIEW_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (sockaddr *)&a, sizeof(a)) != 0) {
        LOG("trackview: bind to port %d failed", TRACKVIEW_PORT);
        close(g_sock);
        g_sock = -1;
        return;
    }
    LOG("trackview: waiting for viewers on UDP port %d", TRACKVIEW_PORT);
}

static void viewer_name(const sockaddr_in &a, char *out, int cap)
{
    char ip[16];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    snprintf(out, cap, "%s:%d", ip, ntohs(a.sin_port));
}

bool trackview_poll(uint64_t now)
{
    if (g_sock < 0)
        return false;
    // recv only after poll says there is something (OpenOrbis' MSG_DONTWAIT and O_NONBLOCK
    // do not work, see alvr_client.cpp).
    for (int guard = 0; guard < 16; guard++) {
        pollfd p{g_sock, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN))
            break;
        uint8_t buf[64];
        sockaddr_in from;
        socklen_t len = sizeof(from);
        const int n = (int)recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr *)&from, &len);
        if (n <= 0 || !trackview_is_hello(buf, n))
            continue;
        int i = 0;
        while (i < g_viewer_count && (g_viewers[i].addr.sin_addr.s_addr != from.sin_addr.s_addr ||
                                      g_viewers[i].addr.sin_port != from.sin_port))
            i++;
        if (i == g_viewer_count) {
            if (g_viewer_count == TRACKVIEW_MAX_VIEWERS)
                continue;
            g_viewers[g_viewer_count++].addr = from;
            char name[24];
            viewer_name(from, name, sizeof(name));
            LOG("trackview: viewer %s connected", name);
        }
        g_viewers[i].last_hello_us = now;
    }
    for (int i = 0; i < g_viewer_count;) {
        if (now - g_viewers[i].last_hello_us > TRACKVIEW_TIMEOUT_US) {
            char name[24];
            viewer_name(g_viewers[i].addr, name, sizeof(name));
            LOG("trackview: viewer %s gone", name);
            g_viewers[i] = g_viewers[--g_viewer_count];
        } else {
            i++;
        }
    }
    return g_viewer_count > 0 && now - g_last_send_us >= TRACKVIEW_SEND_PERIOD_US;
}

void trackview_send(const LobbyView *view, uint64_t now)
{
    uint8_t buf[TRACKVIEW_PACKET_MAX];
    const int n = trackview_pack_state(view, g_seq++, buf, sizeof(buf));
    g_last_send_us = now;
    if (n <= 0)
        return;
    for (int i = 0; i < g_viewer_count; i++)
        sendto(g_sock, buf, n, 0, (sockaddr *)&g_viewers[i].addr, sizeof(g_viewers[i].addr));
}
