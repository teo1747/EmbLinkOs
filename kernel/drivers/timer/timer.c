#include "drivers/timer/timer.h"
#include "drivers/timer/hpet.h"
#include "drivers/timer/pit.h"
#include "arch/x86_64/irq/irq.h"
#include "include/kprintf.h"
#include "mm/kheap.h"
#include <stdint.h>

static volatile uint64_t ticks = 0;
volatile int heap_stress_enable = 0;

/* ---- raw TSC ---- */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- TSC calibration state ---- */
static uint64_t tsc_freq_hz  = 0;   /* TSC ticks per second (0 = not calibrated) */
static uint64_t tsc_base     = 0;   /* TSC at calibration point                  */
static bool     tsc_calibrated = false;

/* ---- IRQ 0 handler (PIT / 18.2 Hz) ---- */
static void timer_handler(void)
{
    ticks++;
}

void timer_init(void)
{
    kprintf("=== Timer init ===\n");
    irq_register(0, timer_handler);
    kprintf("Timer initialized. IRQ0 handler registered.\n");
}

uint64_t timer_get_ticks(void)
{
    return ticks;
}

/* ---- TSC calibration ---- */

void tsc_calibrate(void)
{
    if (tsc_calibrated)
        return;

    const uint32_t window_ms = 10;

    uint64_t t0, t1;

    if (hpet_available()) {
        /* High-precision path: measure TSC ticks over a 10 ms HPET window. */
        t0 = rdtsc();
        hpet_delay_ms(window_ms);
        t1 = rdtsc();
        kprintf("TSC: calibrated against HPET (%u ms window)\n", window_ms);
    } else {
        /* PIT fallback: same window, lower accuracy. */
        t0 = rdtsc();
        pit_delay_ms(window_ms);
        t1 = rdtsc();
        kprintf("TSC: calibrated against PIT (%u ms window — HPET unavailable)\n",
                window_ms);
    }

    uint64_t delta = t1 - t0;
    tsc_freq_hz = delta * (1000 / window_ms);   /* scale 10 ms → 1 s */
    tsc_base    = t1;
    tsc_calibrated = true;

    kprintf("TSC: frequency ~%lu MHz\n", tsc_freq_hz / 1000000ULL);
}

uint64_t tsc_read(void)
{
    return rdtsc();
}

uint64_t tsc_get_freq_hz(void)
{
    return tsc_freq_hz;
}

/* time_get_ns / time_get_us use the formula:
 *   elapsed_ticks = rdtsc() - tsc_base
 *   ns = elapsed_ticks * 1_000_000_000 / tsc_freq_hz
 *
 * To avoid 128-bit division, we split the multiplication:
 *   ns = (elapsed_ticks / tsc_freq_hz) * 1e9
 *      + (elapsed_ticks % tsc_freq_hz) * 1e9 / tsc_freq_hz
 *
 * For practical TSC frequencies (1–5 GHz) and elapsed times up to a few
 * hours, the first term dominates and the division stays in 64 bits. */

uint64_t time_get_ns(void)
{
    if (!tsc_calibrated || tsc_freq_hz == 0)
        return 0;

    uint64_t elapsed = rdtsc() - tsc_base;
    /* integer: elapsed * 1e9 / freq */
    uint64_t sec     = elapsed / tsc_freq_hz;
    uint64_t rem     = elapsed % tsc_freq_hz;
    return sec * 1000000000ULL + rem * 1000000000ULL / tsc_freq_hz;
}

uint64_t time_get_us(void)
{
    if (!tsc_calibrated || tsc_freq_hz == 0)
        return 0;

    uint64_t elapsed = rdtsc() - tsc_base;
    uint64_t sec     = elapsed / tsc_freq_hz;
    uint64_t rem     = elapsed % tsc_freq_hz;
    return sec * 1000000ULL + rem * 1000000ULL / tsc_freq_hz;
}