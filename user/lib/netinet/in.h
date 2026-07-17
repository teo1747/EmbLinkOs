/* netinet/in.h -- EmbLink override header. See sys/socket.h's note: the
 * network is ABSENT; these exist so portable code compiles. The byte-order
 * helpers are REAL (pure arithmetic, nothing to refuse). */
#ifndef _EMBK_NETINET_IN_H
#define _EMBK_NETINET_IN_H

#include <sys/socket.h>
#include <stdint.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr  { in_addr_t s_addr; };
struct in6_addr { uint8_t s6_addr[16]; };

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_NONE      ((in_addr_t)0xffffffff)

#define IPPROTO_IP  0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

/* x86-64 is little-endian; these are the real conversions, not stubs. */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xffU) << 24) | ((x & 0xff00U) << 8) |
           ((x >> 8) & 0xff00U) | (x >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

#endif /* _EMBK_NETINET_IN_H */
