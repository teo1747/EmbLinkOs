/* sys/un.h -- EmbLink override header. See sys/socket.h: no network, no unix
 * sockets either (EmbLink IPC is pipes/channels/endpoints, typed and named). */
#ifndef _EMBK_SYS_UN_H
#define _EMBK_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};

#endif /* _EMBK_SYS_UN_H */
