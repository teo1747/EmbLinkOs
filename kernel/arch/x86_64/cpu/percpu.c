#include "arch/x86_64/cpu/percpu.h"
#include "arch/x86_64/irq/lapic.h"
#include "acpi/acpi.h"
#include "include/kprintf.h"
#include "include/kstring.h"

struct cpu_data cpu_table[MAX_CPUS];
uint32_t cpu_count = 0;

/* APIC ID -> cpu_table[] index. 0xFF ("not yet known") rather than 0, so a
 * lookup before percpu_init_topology() has run is visibly wrong (caught by
 * this_cpu()'s own fallback below) instead of silently aliasing whatever
 * happens to be at index 0. Sized for the full APIC-ID space (a single
 * byte, 0-255), matching ACPI_MAX_CPUS. */
static uint8_t apic_id_to_index[256];
#define APIC_ID_UNKNOWN 0xFFu

static bool topology_ready = false;

void percpu_init_topology(void) {
    memset(apic_id_to_index, APIC_ID_UNKNOWN, sizeof(apic_id_to_index));

    const struct acpi_info *acpi = acpi_get_info();
    uint32_t my_apic_id = lapic_get_id();
    uint32_t n = acpi->cpu_count;

    if (n > MAX_CPUS) {
        /* More CPUs in the MADT than this kernel's per-core arrays are
         * sized for -- use the first MAX_CPUS entries and no more. A clear,
         * documented capacity limit, not a correctness bug: every core
         * that DOES get an index is still fully correct. */
        n = MAX_CPUS;
    }

    if (n == 0) {
        /* No MADT CPU list at all (missing/malformed ACPI tables) -- fall
         * back to a synthetic single-entry topology built from our own
         * APIC ID, since we're demonstrably running on some CPU right now
         * regardless of what ACPI reported. */
        cpu_table[0].apic_id = my_apic_id;
        cpu_table[0].cpu_index = 0;
        cpu_table[0].online = false;
        apic_id_to_index[(uint8_t)my_apic_id] = 0;
        cpu_count = 1;
        topology_ready = true;
        cpu_table[0].online = true;
        kprintf("percpu: no MADT CPU list; assuming single-core (apic_id=%u)\n",
                (unsigned int)my_apic_id);
        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint8_t apic_id = acpi->cpu_apic_ids[i];
        cpu_table[i].apic_id = apic_id;
        cpu_table[i].cpu_index = i;
        cpu_table[i].online = false;
        apic_id_to_index[apic_id] = (uint8_t)i;
    }

    cpu_count = n;

    /* The BSP is always whichever entry matches OUR OWN APIC ID (read just
     * above) -- not assumed to be MADT entry 0, since the spec doesn't
     * guarantee enumeration order puts the boot processor first, only that
     * it's SOME entry in the list. It's already running (that's what
     * "boot processor" means), so mark it online immediately; every AP
     * marks itself online later, in ap_main(). */
    uint8_t bsp_index = apic_id_to_index[(uint8_t)my_apic_id];
    if (bsp_index == APIC_ID_UNKNOWN) {
        /* Our own APIC ID wasn't in the MADT list at all -- shouldn't
         * happen on real ACPI-compliant firmware, but if it does, adopt
         * index 0 as the BSP rather than leaving this_cpu() unable to find
         * itself at all. */
        bsp_index = 0;
        cpu_table[0].apic_id = (uint8_t)my_apic_id;
        cpu_table[0].cpu_index = 0;
        apic_id_to_index[(uint8_t)my_apic_id] = 0;
    }
    cpu_table[bsp_index].online = true;

    topology_ready = true;

    kprintf("percpu: %u core(s) found in MADT, this core (apic_id=%u) is index %u\n",
            (unsigned int)cpu_count, (unsigned int)my_apic_id, (unsigned int)bsp_index);
}

struct cpu_data *this_cpu(void) {
    if (!topology_ready) {
        /* Only reachable during the BSP's own very first gdt_init_bsp()
         * call, which runs before acpi_init()/lapic_init() (see
         * kernel_main's ordering) -- correct by construction, since nothing
         * but the BSP itself can possibly be executing this early. */
        return &cpu_table[0];
    }
    uint32_t apic_id = lapic_get_id();
    uint8_t idx = apic_id_to_index[(uint8_t)apic_id];
    if (idx == APIC_ID_UNKNOWN) {
        /* Should never happen once topology_ready -- every core that could
         * possibly be executing C code was enumerated by
         * percpu_init_topology(). Fall back to the BSP rather than
         * dereferencing a bogus index. */
        return &cpu_table[0];
    }
    return &cpu_table[idx];
}
