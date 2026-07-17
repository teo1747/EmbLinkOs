/* sys/socket.h -- EmbLink override header (newlib ships none).
 *
 * EmbLink userspace has NO network stack -- not "not yet linked", ABSENT. These
 * types and constants exist so that portable code (git's connect.c et al)
 * COMPILES; the functions are defined in syscalls.c as honest ENOSYS refusals,
 * so a `git fetch git://...` fails at the socket() call with a truthful errno
 * while purely local work never comes near them. Nothing here fakes success. */
#ifndef _EMBK_SYS_SOCKET_H
#define _EMBK_SYS_SOCKET_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

/* Big enough for any sockaddr_* in these shims, aligned like the real thing. */
struct sockaddr_storage {
    sa_family_t ss_family;
    char        __ss_pad[126];
} __attribute__((aligned(8)));

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  1
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_ERROR     4
#define SO_KEEPALIVE 9

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#ifdef __cplusplus
extern "C" {
#endif

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int shutdown(int fd, int how);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYS_SOCKET_H */
