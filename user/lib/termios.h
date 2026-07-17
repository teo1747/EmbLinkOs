/* termios.h -- EmbLink override header.
 *
 * newlib SHIPS a termios.h, but it is a one-line `#include <sys/termios.h>`
 * pointing at a file that does not exist for this target -- broken the same
 * way its utime.h is (see user/lib/sys/utime.h). Same cure: override it.
 *
 * EmbLink's console has no termios line discipline; tcgetattr()/tcsetattr()
 * are defined in syscalls.c as honest ENOTTY-style refusals so callers (git's
 * terminal prompting) take their "not a terminal" fallback paths. */
#ifndef _EMBK_TERMIOS_H
#define _EMBK_TERMIOS_H

typedef unsigned char  cc_t;
typedef unsigned int   speed_t;
typedef unsigned int   tcflag_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
};

/* c_lflag bits and c_cc indices callers actually name. */
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008
#define VMIN   6
#define VTIME  5

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#ifdef __cplusplus
extern "C" {
#endif

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_TERMIOS_H */
