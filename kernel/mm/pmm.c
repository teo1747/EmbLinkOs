#include "mm/pmm.h"
#include "drivers/char/serial.h"
#include "arch/x86_64/smp/smp.h"
#include "arch/x86_64/cpu/spinlock.h"

/* Guards pmm_bitmap/free_pages/used_pages -- unused until SMP (Phase 3,
 * docs/architecture/process-and-scheduling.md), same "add the lock, don't
 * build a fancier allocator" call already made for kheap.c's heap_lock. */
static spinlock_t pmm_lock = SPINLOCK_INIT;


static const char *type_names[] = {
    "Unknown",
    "Usable",
    "Reserved",
    "ACPI Reclaimable",
    "ACPI NVS",
    "Bad Memory"
};


extern uint8_t kernel_end[]; // Symbol defined in linker script, marks the end of the kernel in memory
// Bitmap pointer - lives at BITMAP_ADDR and is used to track allocated/free pages. Each bit represents a page (4KB).

static uint8_t *pmm_bitmap = 0; // Will point to our bitmap area in memory
static uint64_t total_pages = 0;  // Total number of pages we can manage
static uint64_t used_pages = 0; // Total number of used pages based on E820 map
static uint64_t free_pages = 0;   // Total number of free pages available for allocation
static uint64_t bitmap_size = 0;    // Size of the bitmap in bytes (total_pages / 8)


static inline void bitmap_set(uint64_t page_index) {
    pmm_bitmap[page_index / 8] |= (1 << (page_index % 8));
}

static inline void bitmap_clear(uint64_t page_index) {
    pmm_bitmap[page_index / 8] &= ~(1 << (page_index % 8));
}

static inline int bitmap_test(uint64_t page_index) {
    return (pmm_bitmap[page_index / 8] & (1 << (page_index % 8))) != 0;
}


void pmm_print_map(void) {
    uint32_t count = *(uint32_t *)KP2V(E820_COUNT_ADDR);
    struct e820_entry *entries = (struct e820_entry *)KP2V(E820_ENTRIES_ADDR);

    serial_write_string("\n=== E820 Memory Map ===\n");
    serial_write_string("entries: ");
    serial_write_hex(count);
    serial_write_string("\n");

    uint64_t total_usable = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct e820_entry *entry = &entries[i];

        serial_write_string("[");
        serial_write_hex(i);
        serial_write_string("]  Base:");
        serial_write_hex(entry->base);
        serial_write_string(", Length:");
        serial_write_hex(entry->length);
        serial_write_string(", Type:");

        if (entry->type >=1 && entry->type <=5) {
           serial_write_string(type_names[entry->type]);
        } else {
           serial_write_string("unknown");
        }
        serial_write_string("\n");

}



}


void pmm_print_stats(void) {
    serial_write_string("\n=== Physical Memory Manager Stats ===\n");
    serial_write_string("Total Pages: ");
    serial_write_hex(total_pages);
    serial_write_string("\nUsed Pages: ");
    serial_write_hex(used_pages);
    serial_write_string("\nFree Pages: ");
    serial_write_hex(free_pages);
    serial_write_string("\n");
}


void pmm_init(void) {
    // For now, we just print the memory map. In a real implementation, we would initialize our physical memory manager here.
    pmm_print_map();

    uint32_t count = *(uint32_t *)KP2V(E820_COUNT_ADDR); // Get the number of E820 entries
    struct e820_entry *entries = (struct e820_entry *)KP2V(E820_ENTRIES_ADDR); // Get pointer to the E820 entries

    // step 1: find the highest addressable memory to determine how many pages we need to manage
    uint64_t highest_address = 0;
    for (uint32_t i = 0; i < count; i++) {
        // only consider usable regions for determining the highest address, since we won't be managing reserved or bad memory
        if (entries[i].type != E820_USABLE) {
            continue;
        }

        uint64_t end = entries[i].base + entries[i].length;
        if (end > highest_address) {
            highest_address = end;
        }
    }

    // step 2: Parse E820 map and mark usable pages as free
    
    total_pages = highest_address  / PAGE_SIZE; 
    bitmap_size = (total_pages + 7) / 8; // Size of bitmap in bytes (round up to nearest byte)

    // step 3: We will place our bitmap at the end of the kernel in memory, which is defined by the linker script as 'kernel_end'. We need to ensure that the bitmap does not overlap with the kernel or any reserved areas.
    pmm_bitmap = (uint8_t *)kernel_end; // Place bitmap right after the kernel in memory

    serial_write_string("\n=== PMM Layout ===\n");
    serial_write_string("Kernel End: ");
    serial_write_hex((uint64_t)kernel_end);
    serial_write_string("\nBitmap at: ");
    serial_write_hex((uint64_t)pmm_bitmap);
    serial_write_string("\nBitmap Size: ");
    serial_write_hex(bitmap_size);
    serial_write_string(" bytes\nTotal pages: ");
    serial_write_hex(total_pages);
    serial_write_string("\n");

    // step 4: Mark all pages as used initially

    for (uint64_t i = 0; i < bitmap_size; i++) {
        pmm_bitmap[i] = 0xFF; // Mark all pages as used (set all bits to 1)
    }
    used_pages = total_pages; // Initially, we consider all pages as used until we parse the E820 map
    free_pages = 0; 


    // step 5: Now go through the E820 map again and mark usable pages as free in our bitmap
    for (uint32_t i = 0; i < count; i++) {

        if(entries[i].type != E820_USABLE) {
            continue; // Skip non-usable regions
        }

        uint64_t start_page = entries[i].base / PAGE_SIZE;
        uint64_t end_page = (entries[i].base + entries[i].length) / PAGE_SIZE;

        for (uint64_t p = start_page; p < end_page && p < total_pages; p++) {
            if (bitmap_test(p)) {
                bitmap_clear(p); // Mark the page as free
                free_pages++;
                used_pages--;
            }
        }
    }

    // step 6: Reserve kernel + bitmap region
    uint64_t bitmap_phys_end = KV2P(pmm_bitmap) + bitmap_size; // Calculate the physical end of the bitmap region
    uint64_t reserve_end_page = (bitmap_phys_end + PAGE_SIZE - 1) / PAGE_SIZE ; // Reserve the entire bitmap region for now

    for (uint64_t p = 0; p <= reserve_end_page && p < total_pages; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p); // Mark the page as reserved 
            free_pages--;
            used_pages++;
        }
        
    }


    // The boot stack now lives in the kernel's own .bss (see kernel/cpu/kentry.asm),
    // so it sits below kernel_end and is already covered by the kernel+bitmap
    // reservation above — no separate boot-stack reservation is needed.

    // step 7: explicitly reserve fixed-address pages that something OUTSIDE
    // this allocator's own bookkeeping needs to own at a known physical
    // address. Today this is only the AP trampoline (kernel/cpu/smp.h) --
    // already covered incidentally by step 6's blanket reservation of
    // everything below kernel_end (which is always > 1MB), but that's an
    // accident of where step 6's loop happens to start (page 0), not a
    // deliberate low-memory carve-out. Reserving it explicitly here means
    // it stays reserved even if step 6 is ever changed to start later.
    pmm_reserve_page(AP_TRAMPOLINE_PHYS);

    pmm_print_stats();

}


// Allocate a single page of physical memory 
// Physical end (exclusive, page-aligned) of the region PMM keeps for the kernel
// image plus its own bitmap. The bitmap lives at kernel_end and is written before
// paging is rebuilt, so vmm_init must map at least this far in the kernel window
// or the first bitmap access after the CR3 switch faults.
uint64_t pmm_reserved_phys_end(void) {
    uint64_t bitmap_phys_end = KV2P(pmm_bitmap) + bitmap_size;
    return (bitmap_phys_end + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
}

uint64_t pmm_alloc_page(void) {
    spin_lock(&pmm_lock);

    for (uint64_t page_index = 0; page_index < total_pages; page_index++) {
        if (!bitmap_test(page_index)) { // If the page is free
            bitmap_set(page_index); // Mark it as used
            free_pages--;
            used_pages++;
            spin_unlock(&pmm_lock);
            return (page_index * PAGE_SIZE); // Return the physical address of the allocated page
        }
    }
    spin_unlock(&pmm_lock);
    return 0; // No free pages available
}


// Free a single page of physical memory
void pmm_free_page(uint64_t phys_addr) {
    spin_lock(&pmm_lock);
    uint64_t page_index = (uint64_t)phys_addr / PAGE_SIZE;
    if (page_index >= total_pages) {
        spin_unlock(&pmm_lock);
        return; // Invalid page index, ignore
    }
    if (bitmap_test(page_index)) { // If the page is currently allocated
        bitmap_clear(page_index); // Mark it as free
        free_pages++;
        used_pages--;
    }
    spin_unlock(&pmm_lock);
}

void pmm_reserve_page(uint64_t phys_addr) {
    spin_lock(&pmm_lock);
    uint64_t page_index = phys_addr / PAGE_SIZE;
    if (page_index >= total_pages) {
        spin_unlock(&pmm_lock);
        return; // Out of range, ignore
    }
    if (!bitmap_test(page_index)) { // Only if not already reserved/used
        bitmap_set(page_index);
        free_pages--;
        used_pages++;
    }
    spin_unlock(&pmm_lock);
}


