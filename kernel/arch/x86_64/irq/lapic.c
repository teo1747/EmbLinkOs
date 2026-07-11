#include "arch/x86_64/irq/lapic.h"
#include "drivers/char/serial.h"
#include "drivers/timer/hpet.h"
#include "acpi/acpi.h"
#include "include/kprintf.h"
#include "mm/vmm.h"
#include "drivers/timer/pit.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/cpu/percpu.h"
#include <stdint.h>


extern void lapic_timer_stub(void); // Forward declaration of the LAPIC timer handler defined in isr.asm

static volatile uint8_t *lapic_base = NULL; // virtual address of local APIC registers
static uint32_t lapic_timer_ticks_per_ms = 0; // calibrated timer ticks per millisecond for the local APIC timer
static volatile uint64_t lapic_ticks = 0;

// Read a 32 bit LAPIC register
static inline uint32_t lapic_read(uint32_t reg){
    return *(volatile uint32_t *)(lapic_base + reg);
}

// Write a 32 bit LAPIC register
static inline void lapic_write(uint32_t reg, uint32_t value){
    *(volatile uint32_t *)(lapic_base + reg) = value;
}

// Read an MSR
static inline uint64_t rdmsr(uint32_t msr){
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Write an MSR
static inline void wrmsr(uint32_t msr, uint64_t value){
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}


#define IA32_APIC_BASE_MSR     0x1B
#define A32_APIC_BASE_ENABLE (1 << 11)


/* The MSR enable bit + spurious-vector register write are PER-CORE
 * hardware state -- every core has its own local APIC and must do this
 * itself. Split out from lapic_init() so an AP (kernel/cpu/smp.c's
 * ap_main()) can call just this part, via lapic_init_this_cpu() below,
 * without repeating the ACPI lookup + vmm_map_mmio() call, both of which
 * must happen exactly once (the LAPIC MMIO window maps to the same
 * physical address on every core -- see percpu.h's comment -- so mapping
 * it again would just waste MMIO_BASE bump-allocator space for no
 * benefit). */
static void lapic_enable_this_core(void) {
    // Enable the local APIC by setting the enable bit in the Spurious Interrupt Vector Register
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base |= A32_APIC_BASE_ENABLE; // Set the enable bit
    wrmsr(IA32_APIC_BASE_MSR, base);

    // Software enable the APIC by setting the spurious interrupt vector and enable bit in the Spurious Interrupt Vector Register
    lapic_write(LAPIC_REG_SPURIOUS, LAPIC_SVR_ENABLE | 0xFF); // Use vector 0xFF for spurious interrupts
}

void lapic_init(void) {

    serial_write_string("\n=== LAPIC INIT ===\n");

    const struct acpi_info *acpi = acpi_get_info();
    if (!acpi->found) {
        kprintf("LAPIC init failed: ACPI tables not found\n");
        return;
    }

    // Map the local APIC registers into virtual memory
    lapic_base = (volatile uint8_t *)vmm_map_mmio(acpi->local_apic_address, 0x1000);
    if (!lapic_base) {
        kprintf("LAPIC init failed: Unable to map LAPIC registers MMIO\n");
        return;
    }
    kprintf("LAPIC; MMIO mapped at %p (phys %p)\n", lapic_base, (void *)acpi->local_apic_address);

    lapic_enable_this_core();
    kprintf("LAPIC: enable, id=%u, version=%x\n",
        (unsigned int)lapic_get_id(),(unsigned int) lapic_read(LAPIC_REG_VERSION) & 0xFF);
}

void lapic_init_this_cpu(void) {
    /* lapic_base is already mapped (by the BSP's lapic_init() call, which
     * always runs first) and is valid on every core -- see this file's
     * lapic_enable_this_core() comment for why. Just the per-core enable
     * bit + spurious vector, nothing else. */
    lapic_enable_this_core();
}

/* Wait for the ICR's own delivery-status bit (bit 12 of the low dword) to
 * clear, meaning the last IPI this core sent has actually been accepted by
 * the bus -- sending a second ICR write before this clears is undefined
 * per the SDM. A bounded spin, not indefinite: on real hardware or a
 * correctly emulated APIC this always clears quickly; looping forever on
 * a hypothetical stuck bit would hang the BSP's whole boot instead of
 * just failing this one AP. */
static void lapic_wait_icr_idle(void) {
    for (int i = 0; i < 100000; i++) {
        if ((lapic_read(LAPIC_REG_ICR_LOW) & (1 << 12)) == 0) {
            return;
        }
        __asm__ volatile ("pause");
    }
}

/* Minimum-wait delay for the gaps in the INIT-SIPI-SIPI sequence below.
 * Millisecond granularity is coarser than the SDM's suggested 200us
 * between the two SIPIs, but waiting longer than the minimum is always
 * safe -- only waiting less would be a problem. Mirrors the existing
 * HPET-preferred-else-PIT pattern lapic_timer_init() already uses. */
static void smp_min_delay(void) {
    if (hpet_available()) {
        hpet_delay_us(200);
    } else {
        pit_delay_ms(1);
    }
}

void lapic_start_ap(uint32_t dest_apic_id, uint64_t trampoline_phys) {
    uint32_t dest = dest_apic_id << LAPIC_ICR_DEST_SHIFT;
    uint8_t sipi_vector = (uint8_t)(trampoline_phys >> 12);

    // 1. INIT IPI (assert)
    lapic_write(LAPIC_REG_ICR_HIGH, dest);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
    lapic_wait_icr_idle();

    // 2. INIT IPI deassert
    lapic_write(LAPIC_REG_ICR_HIGH, dest);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_TRIGGER_LEVEL);
    lapic_wait_icr_idle();

    if (hpet_available()) {
        hpet_delay_ms(10);
    } else {
        pit_delay_ms(10);
    }

    // 3. First SIPI
    lapic_write(LAPIC_REG_ICR_HIGH, dest);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_DELIVERY_STARTUP | sipi_vector);
    lapic_wait_icr_idle();
    smp_min_delay();

    // 4. Second SIPI (per SDM recommendation for older processors; a
    // harmless no-op extra nudge on anything modern enough to have
    // started from the first one already).
    lapic_write(LAPIC_REG_ICR_HIGH, dest);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_DELIVERY_STARTUP | sipi_vector);
    lapic_wait_icr_idle();
}


void lapic_send_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0); // Writing any value to the EOI register signals end of interrupt
}


uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_REG_ID) >> 24; // APIC ID is in the high byte of the ID register
}


void lapic_timer_init(uint8_t vector) {

    serial_write_string("\n=== LAPIC timer calibration ===\n");
    // Calibrate the LAPIC timer against the HPET (preferred) or PIT
    const uint32_t calibration_time_ms = 10;

    // Start the LAPIC timer in one-shot mode with a large initial count
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_REG_TIMER_DIVIDE); // Divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_MASKED); // Max initial count
    lapic_write(LAPIC_REG_TIMER_INIT, 0XFFFFFFFF); // Set the interrupt vector (unmasked, one-shot)

    // Wait for the timer to count down for the calibration time
    if (hpet_available()) {
        hpet_delay_ms(calibration_time_ms);
        kprintf("LAPIC timer: calibrating against HPET\n");
    } else {
        pit_delay_ms(calibration_time_ms);
        kprintf("LAPIC timer: calibrating against PIT (HPET unavailable)\n");
    }

    // Stop the timer by masking, read how far it counted
    uint32_t current = lapic_read(LAPIC_REG_TIMER_CURRENT);
    uint32_t elapsed = 0xFFFFFFFF - current; // Calculate elapsed ticks
     
    // Read the current count from the LAPIC timer
    lapic_timer_ticks_per_ms = elapsed / calibration_time_ms;

    kprintf("LAPIC timer: %u ticks in 10ms => %u ticks/ms\n",
        (unsigned int)elapsed, (unsigned int)lapic_timer_ticks_per_ms);

    // now configure periodic mode at 100hz
    uint32_t initial_count = lapic_timer_ticks_per_ms * 10; // 10 ms per tick for 100 Hz
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_REG_TIMER_DIVIDE); // Divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_PERIODIC | vector ); // Set the interrupt vector and periodic mode
    lapic_write(LAPIC_REG_TIMER_INIT, initial_count); // Set the initial count for 100 Hz
    kprintf("LAPIC timer: configured for 100 Hz with initial (count= %u), vector=%u \n", (unsigned int)initial_count, (unsigned int)vector);

    extern void idt_set_entry(uint8_t vector, uint64_t handler, uint8_t type_attr);
    idt_set_entry(vector, (uint64_t)lapic_timer_stub, 0x8E); // Set the LAPIC timer handler in the IDT
}

void lapic_timer_handler(void){
    /* Once every core has its own LAPIC timer firing independently (SMP),
     * incrementing this SHARED counter from every core's handler would
     * silently change its meaning from "wall-clock ticks" to "sum of every
     * core's ticks" -- e.g. selftest_wait_ticks() (process.c) would need a
     * different tick budget depending on how many cores happen to be
     * online. Keep it a true wall clock: only the BSP's own timer
     * advances it. Every core (BSP and every AP) still unconditionally
     * calls schedule() below on its OWN tick -- that part doesn't change. */
    if (this_cpu_is_bsp()) {
        lapic_ticks++; // Increment the LAPIC timer tick count
    }
    lapic_send_eoi(); // Acknowledge the timer interrupt BEFORE any reschedule
                      // below, so the LAPIC can keep delivering interrupts to
                      // whichever process ends up running next.

    /* Timer-driven preemption (docs/architecture/process-and-scheduling.md
     * §13 Phase B): give every READY process a turn every tick (100 Hz, a
     * 10 ms quantum) even if the current one never calls sys_yield/sys_exit.
     * schedule() is a cheap no-op if there's no process yet (current_process
     * == NULL) or nothing else is runnable — same mechanism sys_exit already
     * uses to reach a different process's saved context via
     * kernel_ctx_switch, just triggered by the timer instead of a syscall. */
    extern void schedule(void);
    schedule();
}

uint64_t lapic_timer_get_ticks(void) {
    return lapic_ticks;
}