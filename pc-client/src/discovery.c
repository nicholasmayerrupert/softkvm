#include "discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISCOVERY_PORT 47901
static const char REQUEST[] = "SOFTKVM_DISCOVER";
static const char REPLY[] = "SOFTKVM_HOST";

static void send_to(SOCKET s, uint32_t addr_be) {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(DISCOVERY_PORT);
    to.sin_addr.s_addr = addr_be;
    sendto(s, REQUEST, (int)strlen(REQUEST), 0, (struct sockaddr *)&to, sizeof to);
}

/// Sends to 255.255.255.255 and to each adapter's directed broadcast address, since Windows
/// only sends the limited broadcast out of one interface when several are up.
static void broadcast_all(SOCKET s) {
    send_to(s, INADDR_BROADCAST);
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *addrs = malloc(size);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, addrs, &size) == ERROR_BUFFER_OVERFLOW) {
        addrs = realloc(addrs, size);
        GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, addrs, &size);
    }
    for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
            int prefix = u->OnLinkPrefixLength;
            if (prefix <= 0 || prefix >= 31) continue;
            uint32_t ip = ntohl(sin->sin_addr.s_addr);
            uint32_t mask = prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
            send_to(s, htonl(ip | ~mask));
        }
    }
    free(addrs);
}

int discovery_run(AppHost *out, int max, int timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    bind(s, (struct sockaddr *)&any, sizeof any);

    int n = 0;
    DWORD start = GetTickCount();
    DWORD last_send = 0;
    for (int sends = 0;;) {
        DWORD now = GetTickCount();
        if ((int)(now - start) >= timeout_ms) break;
        if (sends < 3 && (sends == 0 || now - last_send >= 400)) {  // a few tries; UDP can drop
            broadcast_all(s);
            last_send = now;
            sends++;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        struct timeval tv = {0, 100 * 1000};
        if (select(0, &fds, NULL, NULL, &tv) <= 0) continue;
        char buf[256];
        struct sockaddr_in from;
        int fromlen = sizeof from;
        int len = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fromlen);
        // "SOFTKVM_HOST", u16 port, u16 protocol, u8 platform, 32-byte key, u8 len, name
        int rl = (int)strlen(REPLY);
        if (len < rl + 2 + 2 + 1 + 32 + 1 || memcmp(buf, REPLY, rl) != 0) continue;
        const unsigned char *p = (const unsigned char *)buf + rl;
        AppHost h = {0};
        h.port = p[0] | p[1] << 8;
        h.proto = p[2] | p[3] << 8;
        h.platform = p[4];
        memcpy(h.pub, p + 5, 32);
        int name_len = p[37];
        if (name_len > len - rl - 38) name_len = len - rl - 38;
        if (name_len > (int)sizeof h.name - 1) name_len = sizeof h.name - 1;
        memcpy(h.name, p + 38, name_len);
        inet_ntop(AF_INET, &from.sin_addr, h.addr, sizeof h.addr);
        int dup = 0;
        for (int i = 0; i < n; i++) dup |= !memcmp(out[i].pub, h.pub, 32);
        if (!dup && n < max) out[n++] = h;
    }
    closesocket(s);
    return n;
}
