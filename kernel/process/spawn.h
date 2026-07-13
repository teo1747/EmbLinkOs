// shared between process.c and spawn.c, so they can both see the same struct
#include <stdint.h>


#define SPAWN_ARGV_MAX             16          // max number of argv[] entries (including the NULL terminator) a spawn() can pass to a child process
#define SPAWN_ARGV_BYTES_MAX       1024        // total bytes of argv[] strings (including NULL terminators) 
#define SPAWN_ACTIONS_MAX           8          // max number of spawn actions (file descriptor remaps, etc.)
#define SPAWN_ACTION_PATH_MAX      256         // Deliberately seperate from syscall.c's
                                               // SYSCALL_PATH_MAX -- process.h shouldn't
                                               // depend on a #define private to syscall.c

#define SPAWN_ACTION_OPEN            1         // open path onto target_fd in the child
#define SPAWN_ACTION_INHERIT_SURFACE 2         // dup+map a surface into the child (minimal
                                               // handle_transfer -- EmbLink UI Piece 1). For
                                               // this kind, `target_fd` holds the PARENT's
                                               // surface obj-handle; the child receives it at
                                               // its first free obj_handle slot (0 for a fresh
                                               // child), already mapped into its address space.
#define SPAWN_ACTION_INHERIT_CHANNEL 3         // MOVE a channel end into the child (Layer A):
                                               // `target_fd` holds the PARENT's channel-end
                                               // obj-handle; the parent gives it up and the
                                               // child receives it at its first free slot. The
                                               // bootstrap for two-process channel tests.

struct spawn_file_action {
    uint8_t kind;                       // SPAWN_ACTION_OPEN, etc.
    int target_fd;                      // SPAWN_ACTION_OPEN: fd to remap to (e.g. 0 for stdin).
                                        // SPAWN_ACTION_INHERIT_SURFACE: parent's surface handle.
    char path[SPAWN_ACTION_PATH_MAX];   // NUL-terminated path for SPAWN_ACTION_OPEN
    int flags;                          // O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, etc. for SPAWN_ACTION_OPEN
    int mode;                           // 0o644, 0o600, etc. for SPAWN_ACTION_OPEN

};

