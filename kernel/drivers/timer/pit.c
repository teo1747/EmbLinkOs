#include "drivers/timer/pit.h"
#include "include/io.h"

#define PIT_FREQUENCY 1193182  // PIT input clock frequency in Hz

// One one-shot cycle can count at most 0xFFFF ticks before the 16-bit
// counter wraps, which bounds a single cycle to ~54.9ms at this frequency.
#define PIT_MAX_COUNT       0xFFFFu
#define PIT_MAX_CYCLE_US    ((uint64_t)PIT_MAX_COUNT * 1000000ULL / PIT_FREQUENCY)

// PIT channel 2 is gated by bit 0 of port 0x61. We will use channel 2 in mode 0 (one-shot) to implement a microsecond delay function.
// can be read at bit 5 of port 0x61 (0 = ready, 1 = counting)

// Run one one-shot cycle of exactly `count` PIT ticks (count must be <=
// PIT_MAX_COUNT -- not re-checked here, only pit_delay_us below calls this
// and it already clamps every cycle to that bound).
static void pit_oneshot_ticks(uint16_t count)
{
    // Enable channel 2 gate by setting bit 0 of port 0x61
    uint8_t port61 = inb(0x61);
    port61 |= (port61 & ~0x02) | 0x01; // Set bit 0 to enable gate
    outb(0x61, port61);

    // Command byte: channel 2, access mode lobyte/hibyte, mode 0 (one-shot), binary counting
    outb(0x43, 0xB0);
    outb(0x42, count & 0xFF);         // low byte
    outb(0x42, (count >> 8) & 0xFF);  // high byte

    // Restart the counter by toggling bit 0 of port 0x61
    port61 = inb(0x61) & ~0x01; // Clear bit 0 to disable gate
    outb(0x61, port61);
    port61 |= 0x01; // Set bit 0 to enable gate and start counting
    outb(0x61, port61);

    // Wait for the PIT to finish counting by polling bit 5 of port 0x61
    while (!(inb(0x61) & 0x20)) {
        // Still counting
    }
}

// Busywait for approximately `us` microseconds, looping one-shot cycles as
// needed. A single PIT one-shot cycle can only count up to 0xFFFF ticks
// (~54.9ms at this frequency, PIT_MAX_CYCLE_US above) -- `pit_delay_ms`
// used to silently CLAMP any request past that to one 0xFFFF-tick cycle
// instead of looping, so `pit_delay_ms(500)` actually delayed ~55ms, not
// 500ms, for every single caller (found while writing a selftest that
// needed a real multi-hundred-ms delay and got one that was 4x too short).
// This is the fix: run as many full-length cycles as fit, then one final
// shorter cycle for the remainder.
static void pit_delay_us(uint64_t us)
{
    while (us > 0) {
        uint64_t chunk_us = (us > PIT_MAX_CYCLE_US) ? PIT_MAX_CYCLE_US : us;
        uint32_t count = (uint32_t)(chunk_us * PIT_FREQUENCY / 1000000ULL);
        if (count == 0) count = 1;         // a nonzero remainder must still count at least 1 tick
        if (count > PIT_MAX_COUNT) count = PIT_MAX_COUNT;
        pit_oneshot_ticks((uint16_t)count);
        us -= chunk_us;
    }
}

void pit_delay_ms(uint32_t ms)
{
    pit_delay_us((uint64_t)ms * 1000ULL);
}
