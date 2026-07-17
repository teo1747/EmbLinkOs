/* netdb.h -- EmbLink override header. See sys/socket.h's note: name resolution
 * is ABSENT (no network). getaddrinfo/gethostbyname are defined in syscalls.c
 * as honest failures (EAI_FAIL / NULL), so callers take their error paths. */
#ifndef _EMBK_NETDB_H
#define _EMBK_NETDB_H

#include <sys/socket.h>

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct servent {
    char  *s_name;
    char **s_aliases;
    int    s_port;
    char  *s_proto;
};

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08
#define AI_ADDRCONFIG  0x20

#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02

#define EAI_BADFLAGS  -1
#define EAI_NONAME    -2
#define EAI_AGAIN     -3
#define EAI_FAIL      -4
#define EAI_FAMILY    -6
#define EAI_SOCKTYPE  -7
#define EAI_SERVICE   -8
#define EAI_MEMORY    -10
#define EAI_SYSTEM    -11

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4

#ifdef __cplusplus
extern "C" {
#endif

int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
int  getnameinfo(const struct sockaddr *sa, socklen_t salen,
                 char *host, socklen_t hostlen,
                 char *serv, socklen_t servlen, int flags);
const char *gai_strerror(int errcode);
struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *proto);

/* Legacy resolver error channel (gethostbyname's errno). Kept because callers
 * (git's connect.c) print hstrerror(h_errno) on failure -- and failure is the
 * only thing our resolver does. */
extern int h_errno;
const char *hstrerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_NETDB_H */
