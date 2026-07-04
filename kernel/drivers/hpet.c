#include "hpet.h"
#include "../acpi/acpi.h"
#include "../mm/vmm.h"
#include "../include/kprintf.h"
#include <stdint.h>

/* ---- HPET register offsets (from the MMIO base) ---- */
#define HPET_REG_CAPS        0x000   /* General Capabilities and ID (64-bit) */
#define HPET_REG_CONFIG      0x010   /* General Configuration (64-bit)       */
#define HPET_REG_COUNTER     0x0F0   /* Main Counter Value (64-bit)          */

/* General Capabilities register fields */
#define HPET_CAPS_PERIOD_SHIFT 32    /* bits[63:32] = timer period in fs     */
#define HPET_CAPS_PERIOD_MASK  0xFFFFFFFF00000000ULL

/* General Configuration register bits */
#define HPET_CFG_ENABLE      (1ULL << 0)   /* overall enable; main counter runs */
#define HPET_CFG_LEGACY_IRQ  (1ULL << 1)   /* legacy replacement IRQ routing    */

/* ---- module state ---- */
static volatile uint64_t *hpet_base = NULL;  /* virtual address of HPET MMIO  */
static uint64_t           hpet_period  = 0;  /* femtoseconds per tick          */
static bool               hpet_ready   = false;

/* ---- helpers ---- */
static inline uint64_t hpet_reg_read(uint32_t off)
{
    return *(volatile uint64_t *)((volatile uint8_t *)hpet_base + off);
}

static inline void hpet_reg_write(uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)((volatile uint8_t *)hpet_base + off) = val;
}

/* ---- public API ---- */

bool hpet_init(void)
{
    const struct acpi_info *acpi = acpi_get_info();
    if (!acpi || !acpi->hpet_found || !acpi->hpet_address) {
        kprintf("HPET: not present in ACPI tables\n");
        return false;
    }

    /* Map the 1 KB HPET register block into virtual memory (uncacheable). */
    uint64_t virt = vmm_map_mmio(acpi->hpet_address, 0x400);
    if (!virt) {
        kprintf("HPET: MMIO mapping failed\n");
        return false;
    }
    hpet_base = (volatile uint64_t *)virt;

    /* Read capabilities: extract and validate the timer period. */
    uint64_t caps = hpet_reg_read(HPET_REG_CAPS);
    hpet_period = caps >> HPET_CAPS_PERIOD_SHIFT;   /* femtoseconds per tick */

    /* ACPI spec §2.3.7: period must be ≤ 100 000 000 fs (≥ 10 MHz) and
     * must not be 0. Reject obviously bogus values. */
    if (hpet_period == 0 || hpet_period > 100000000ULL) {
        kprintf("HPET: invalid period %lu fs — disabling\n", hpet_period);
        hpet_base  = NULL;
        hpet_period = 0;
        return false;
    }

    /* Disable the counter, then set the main counter to 0 and re-enable.
     * Legacy replacement routing is left off so the LAPIC/IO-APIC wiring
     * we already have is not disturbed. */
    hpet_reg_write(HPET_REG_CONFIG, 0);          /* stop counter */
    hpet_reg_write(HPET_REG_COUNTER, 0);          /* reset counter */
    hpet_reg_write(HPET_REG_CONFIG, HPET_CFG_ENABLE);  /* start counter */

    uint64_t freq_mhz_tenths = 1000000000000000ULL / hpet_period / 100000;
    kprintf("HPET: mapped at %p  period=%lu fs  freq~%lu.%lu MHz\n",
            (void *)virt,
            hpet_period,
            freq_mhz_tenths / 10,
            freq_mhz_tenths % 10);

    hpet_ready = true;
    return true;
}

bool hpet_available(void)
{
    return hpet_ready;
}

uint64_t hpet_read_counter(void)
{
    if (!hpet_ready)
        return 0;
    return hpet_reg_read(HPET_REG_COUNTER);
}

uint64_t hpet_period_fs(void)
{
    return hpet_period;
}

void hpet_delay_us(uint64_t us)
{
    if (!hpet_ready || us == 0)
        return;

    /* 1 us = 1_000_000_000 fs, so:
     *   ticks_needed = us * (1e9 fs / hpet_period fs-per-tick)
     * For hpet_period in [10^6, 10^8] fs the per-us quotient is 10..1000,
     * which fits 64 bits for any reasonable `us`. (This used 1e12 before —
     * a fs-vs-ps mixup that made every HPET delay 1000x too long and threw
     * the LAPIC timer calibration off by the same factor.) */
    uint64_t ticks_per_us = 1000000000ULL / hpet_period;  /* floor */
    if (ticks_per_us == 0)
        ticks_per_us = 1;

    uint64_t target = hpet_reg_read(HPET_REG_COUNTER) + us * ticks_per_us;
    while (hpet_reg_read(HPET_REG_COUNTER) < target)
        ;
}

void hpet_delay_ms(uint32_t ms)
{
    hpet_delay_us((uint64_t)ms * 1000ULL);
}
