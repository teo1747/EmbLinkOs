#ifndef __TIMER_H__
#define __TIMER_H__

#include <stdint.h>

/* Install the timer IRQ handler (IRQ0 / PIT keeps running at BIOS default). */
void timer_init(void);

/* Get the raw LAPIC 100-Hz tick count (used by the heartbeat loop). */
uint64_t timer_get_ticks(void);

/*
 * TSC-based high-resolution time.
 *
 * tsc_calibrate() measures the TSC frequency against the HPET (preferred)
 * or the PIT (fallback) over a ~10 ms window.  Must be called once, after
 * hpet_init() and lapic_timer_init().  Subsequent calls are no-ops.
 *
 * time_get_ns() / time_get_us() return nanoseconds / microseconds elapsed
 * since tsc_calibrate() was called.  Both are lock-free; they execute a
 * single rdtsc instruction in the hot path.
 *
 * tsc_read() exposes the raw 64-bit TSC value for callers that need it
 * (e.g., the LAPIC timer can stamp the TSC at every tick for profiling).
 */
void     tsc_calibrate(void);
uint64_t tsc_read(void);
uint64_t time_get_ns(void);
uint64_t time_get_us(void);

/* Return the calibrated TSC frequency in Hz (0 before calibration). */
uint64_t tsc_get_freq_hz(void);

#endif /* __TIMER_H__ */