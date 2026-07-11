#ifndef __HPET_H__
#define __HPET_H__

#include "include/types.h"
#include <stdint.h>

/*
 * HPET (High Precision Event Timer) driver.
 *
 * The HPET provides a single wide monotonic counter driven by a fixed-
 * frequency oscillator (typically 10–25 MHz, described via ACPI). It is
 * used here for two purposes:
 *
 *   1. Busy-wait delays during early boot calibration, replacing the
 *      less-accurate PIT channel 2 trick.
 *   2. Providing a reference for calibrating the TSC frequency so the
 *      timer layer can serve nanosecond timestamps via rdtsc alone (no
 *      MMIO read in the hot path).
 *
 * The HPET main counter is 64-bit and wraps after ~570 years at 10 MHz,
 * so overflow is not a practical concern.
 */

/* Discover the HPET via ACPI, map its MMIO block, and enable the
 * main counter.  Returns true on success, false if the HPET was not
 * found in the ACPI tables or the MMIO mapping failed.
 *
 * Must be called after acpi_init() and vmm/heap are live. */
bool hpet_init(void);

/* True if hpet_init() succeeded. */
bool hpet_available(void);

/* Read the 64-bit main counter.  Callers must hold no spinlock that
 * must not be interrupted by a stale read — this is a pure MMIO read
 * with no side-effects. */
uint64_t hpet_read_counter(void);

/* Return the HPET tick period in femtoseconds (fs per tick).
 * Value comes from the hardware General Capabilities register.
 * Typical range: 40 000 000 (25 MHz) – 100 000 000 (10 MHz). */
uint64_t hpet_period_fs(void);

/* Stall for at least `us` microseconds by spinning on the main counter.
 * Requires hpet_available() == true.  Falls back to nothing if HPET is
 * absent (caller should check). */
void hpet_delay_us(uint64_t us);

/* Stall for at least `ms` milliseconds (convenience wrapper). */
void hpet_delay_ms(uint32_t ms);

#endif /* __HPET_H__ */
