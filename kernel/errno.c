#include "include/errno.h"

const char *embk_strerror(int err) {
    // Accept either the positive code or the negative return form
    if (err < 0) err = -err;

    switch (err) {
        case EMBK_OK:         return "success";
        case EMBK_EPERM:         return "operation not permitted";
        case EMBK_ENOENT:        return "no such file or directory";
        case EMBK_EIO:           return "I/O error";
        case EMBK_ENXIO:         return "no such device or address";
        case EMBK_ENOEXEC:       return "exec format error";
        case EMBK_EBADF:         return "bad file descriptor";
        case EMBK_ECHILD:        return "no child processes";
        case EMBK_EAGAIN:        return "try again";
        case EMBK_ENOMEM:        return "out of memory";
        case EMBK_EACCES:        return "permission denied";
        case EMBK_EFAULT:        return "bad address";
        case EMBK_EBUSY:         return "device or resource busy";
        case EMBK_EEXIST:        return "file exists";
        case EMBK_EXDEV:         return "cross-device link";
        case EMBK_ENODEV:        return "no such device";
        case EMBK_ENOTDIR:       return "not a directory";
        case EMBK_EISDIR:        return "is a directory";
        case EMBK_EINVAL:        return "invalid argument";
        case EMBK_ENFILE:        return "too many open files in system";
        case EMBK_EMFILE:        return "too many open files";
        case EMBK_ENOSPC:        return "no space left on device";
        case EMBK_ESPIPE:        return "illegal seek";
        case EMBK_EROFS:         return "read-only file system";
        case EMBK_EMLINK:        return "too many links";
        case EMBK_ERANGE:        return "out of range";
        case EMBK_EDEADLK:       return "resource deadlock avoided";
        case EMBK_ENAMETOOLONG:  return "file name too long";
        case EMBK_ENOLCK:        return "no locks available";
        case EMBK_ENOSYS:        return "function not implemented";
        case EMBK_ENOTEMPTY:     return "directory not empty";
        case EMBK_ELOOP:         return "too many levels of symbolic links";
        case EMBK_ENODATA:       return "no data available";
        case EMBK_EOVERFLOW:     return "value too large for defined data type";
        case EMBK_EILSEQ:        return "illegal byte sequence";
        case EMBK_ETIMEDOUT:     return "operation timed out";
        case EMBK_ECANCELED:     return "operation cancelled";
        default:                 return "unknown error";
    }
}