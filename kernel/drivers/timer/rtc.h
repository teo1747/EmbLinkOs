#ifndef __RTC_H__
#define __RTC_H__

#include <stdint.h>

// Read the CMOS real-time clock and return the current wall-clock time as
// seconds since the Unix epoch (1970-01-01T00:00:00Z). This is the only
// source of calendar time this kernel has -- everything that wants a real
// timestamp (EMBKFS inode times, snapshot records, ...) goes through this,
// directly or via rtc_now_ns() below.
//
// Assumes the RTC battery/century is set to a year in [2000, 2099] -- the
// CMOS year register is only two BCD/binary digits wide and there is no
// universally-agreed century register location across chipsets, so (like
// most hobby-OS RTC drivers, and QEMU's own default CMOS clock) this reads
// "YY" and adds 2000. Revisit only if this kernel is still running in 2100.
uint64_t rtc_now_unix(void);

// rtc_now_unix() * 1,000,000,000 -- matches EMBKFS's nanosecond-precision
// inode timestamp fields (embkfs.h's atime/mtime/ctime/btime), but carries
// NO real sub-second precision: a CMOS RTC's own resolution is one second.
// Two calls within the same wall-clock second return the identical value.
// This is stated plainly here so a caller never assumes sub-second
// ordering it doesn't actually have.
uint64_t rtc_now_ns(void);

// Selftest: sanity-range check (a plausible year) plus a monotonicity check
// across a real, PIT-timed delay. Returns 0 on success, -1 on failure --
// matches this codebase's other `*_run_selftests()` functions (e.g.
// cpu/rwlock.c's rwlock_run_selftests()).
int rtc_run_selftests(void);

#endif /* __RTC_H__ */
