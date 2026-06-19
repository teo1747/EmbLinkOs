#ifndef _ERRNO_H
#define _ERRNO_H

// EmBlink OS error codes.
//
// Convention: functions that can fail in distinguishable ways return int,
// where 0 (EMBK_OK) means success and a NEGATIVE value means a specific
// error. Callers test `if (ret < 0)` for failure, or compare against a
// specific code to react to a particular error.
//
// Values mirror POSIX errno semantics (same MEANINGS as the standard
// E* codes) so that a future libc port maps cleanly: kernel returns
// -EMBK_ENOENT, libc translates to errno = ENOENT, programs see the
// expected value. The numeric values match Linux's errno where it costs
// nothing, easing eventual compatibility.

#define EMBK_OK            0    // success

#define EMBK_EPERM         1    // operation not permitted
#define EMBK_ENOENT        2    // no such file or directory
#define EMBK_EIO           5    // I/O error (hardware read/write failed)
#define EMBK_ENXIO         6    // no such device or address
#define EMBK_EBADF         9    // bad file descriptor
#define EMBK_EAGAIN       11    // try again / resource temporarily unavailable
#define EMBK_ENOMEM       12    // out of memory
#define EMBK_EACCES       13    // permission denied
#define EMBK_EFAULT       14    // bad address (invalid pointer)
#define EMBK_EBUSY        16    // device or resource busy
#define EMBK_EEXIST       17    // file already exists
#define EMBK_ENODEV       19    // no such device
#define EMBK_ENOTDIR      20    // not a directory
#define EMBK_EISDIR       21    // is a directory
#define EMBK_EINVAL       22    // invalid argument
#define EMBK_ENOSPC       28    // no space left on device
#define EMBK_EROFS        30    // read-only file system
#define EMBK_ERANGE       34    // result/argument out of range
#define EMBK_ENOSYS       38    // function not implemented
#define EMBK_ENOTEMPTY    39    // directory not empty
#define EMBK_ETIMEDOUT   110    // operation timed out

// Helper: turn an error code into a short human-readable string.
// Returns a static string; never NULL.
const char *embk_strerror(int err);

#endif /* _ERRNO_H */