/* sys/ioctl.h -- EmbLink override header (newlib ships none).
 *
 * One caller matters today: git asks TIOCGWINSZ for the terminal size to wrap
 * its output. ioctl() is defined in syscalls.c and REFUSES (ENOSYS) -- the
 * console has no winsize protocol yet, and a fabricated 80x24 would be a guess
 * dressed as a measurement. Callers fall back to their defaults honestly. */
#ifndef _EMBK_SYS_IOCTL_H
#define _EMBK_SYS_IOCTL_H

#define TIOCGWINSZ 0x5413

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYS_IOCTL_H */
