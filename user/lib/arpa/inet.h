/* arpa/inet.h -- EmbLink override header. Declarations; syscalls.c defines the
 * string converters for real (pure text work, nothing network about them). */
#ifndef _EMBK_ARPA_INET_H
#define _EMBK_ARPA_INET_H

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_ARPA_INET_H */
