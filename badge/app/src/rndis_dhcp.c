/*
 * File: app/src/rndis_dhcp.c
 * Purpose: Minimal DHCP server for the RNDIS usb0 interface.
 *
 * The device uses 192.168.42.1 on usb0 and hands 192.168.42.2 to the host,
 * so Windows/Linux do not need a manually configured IP to open the overlay
 * web UI.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

struct dhcp_packet {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static uint8_t option_message_type(const struct dhcp_packet *pkt)
{
    const uint8_t *opt = pkt->options;
    const uint8_t *end = pkt->options + sizeof(pkt->options);

    while (opt < end && *opt != 255) {
        uint8_t code = *opt++;
        uint8_t len;
        if (code == 0)
            continue;
        if (opt >= end)
            break;
        len = *opt++;
        if (opt + len > end)
            break;
        if (code == 53 && len >= 1)
            return opt[0];
        opt += len;
    }
    return 0;
}

static void build_reply(const struct dhcp_packet *req, struct dhcp_packet *rep,
                        uint8_t type)
{
    uint8_t *o;

    memset(rep, 0, sizeof(*rep));
    rep->op = 2;
    rep->htype = 1;
    rep->hlen = req->hlen ? req->hlen : 6;
    rep->xid = req->xid;
    rep->flags = req->flags;
    rep->yiaddr = inet_addr("192.168.42.2");
    rep->siaddr = inet_addr("192.168.42.1");
    memcpy(rep->chaddr, req->chaddr, sizeof(rep->chaddr));
    rep->magic = htonl(0x63825363);

    o = rep->options;
    *o++ = 53;
    *o++ = 1;
    *o++ = type;
    *o++ = 54;
    *o++ = 4;
    memcpy(o, &rep->siaddr, 4);
    o += 4;
    *o++ = 51;
    *o++ = 4;
    *o++ = 0;
    *o++ = 0;
    *o++ = 0x0e;
    *o++ = 0x10; /* 3600 seconds */
    *o++ = 1;
    *o++ = 4;
    {
        uint32_t mask = inet_addr("255.255.255.0");
        memcpy(o, &mask, 4);
        o += 4;
    }
    *o++ = 3;
    *o++ = 4;
    memcpy(o, &rep->siaddr, 4);
    o += 4;
    *o++ = 6;
    *o++ = 4;
    memcpy(o, &rep->siaddr, 4);
    o += 4;
    *o++ = 255;
}

int main(int argc, char **argv)
{
    const char *ifname = argc > 1 ? argv[1] : "usb0";
    int fd;
    int one = 1;
    struct sockaddr_in addr;
    struct dhcp_packet req;
    struct dhcp_packet rep;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("rndis_dhcp: socket");
        return 1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DHCP_SERVER_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("rndis_dhcp: bind");
        close(fd);
        return 1;
    }

    printf("rndis_dhcp: serving 192.168.42.2 on %s\n", ifname);
    while (g_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, &req, sizeof(req), 0,
                             (struct sockaddr *)&from, &from_len);
        uint8_t type;
        uint8_t reply_type;

        if (n < (ssize_t)(sizeof(req) - sizeof(req.options)))
            continue;
        if (req.op != 1 || req.magic != htonl(0x63825363))
            continue;
        type = option_message_type(&req);
        if (type == 1)
            reply_type = 2; /* DHCPOFFER */
        else if (type == 3)
            reply_type = 5; /* DHCPACK */
        else
            continue;

        build_reply(&req, &rep, reply_type);
        {
            struct sockaddr_in dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            dst.sin_port = htons(DHCP_CLIENT_PORT);
            (void)sendto(fd, &rep, sizeof(rep), 0,
                         (struct sockaddr *)&dst, sizeof(dst));
        }
    }

    close(fd);
    return 0;
}
