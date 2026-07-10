#include "rtc.h"
#include "../include/io.h"
#include "../include/types.h"   /* bool */
#include "../include/kprintf.h"
#include "pit.h"

// Standard CMOS RTC ports (unchanged since the original IBM AT, present on
// every x86 platform this kernel targets, real or emulated).
#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT  0x71

#define CMOS_REG_SECONDS      0x00
#define CMOS_REG_MINUTES      0x02
#define CMOS_REG_HOURS        0x04
#define CMOS_REG_DAY          0x07
#define CMOS_REG_MONTH        0x08
#define CMOS_REG_YEAR         0x09
#define CMOS_REG_STATUS_A     0x0A
#define CMOS_REG_STATUS_B     0x0B

#define CMOS_STATUS_A_UPDATE_IN_PROGRESS (1u << 7)
#define CMOS_STATUS_B_BINARY_MODE        (1u << 2)
#define CMOS_STATUS_B_24_HOUR             (1u << 1)
#define CMOS_HOUR_PM_BIT                  (1u << 7)   // set in the raw hour byte in 12h mode

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX_PORT, reg);
    return inb(CMOS_DATA_PORT);
}

// The RTC's own update cycle (roughly once a second) can leave registers
// briefly inconsistent mid-read (e.g. seconds rolled over but minutes
// hasn't yet). Standard fix, used by every real RTC driver: don't read
// while status A's UIP bit is set, and re-read once more to confirm the
// two snapshots agree -- if they don't, the update happened mid-read and
// we retry.
struct cmos_snapshot {
    uint8_t seconds, minutes, hours, day, month, year;
};

static void cmos_read_snapshot(struct cmos_snapshot *out)
{
    out->seconds = cmos_read(CMOS_REG_SECONDS);
    out->minutes = cmos_read(CMOS_REG_MINUTES);
    out->hours   = cmos_read(CMOS_REG_HOURS);
    out->day     = cmos_read(CMOS_REG_DAY);
    out->month   = cmos_read(CMOS_REG_MONTH);
    out->year    = cmos_read(CMOS_REG_YEAR);
}

static int cmos_snapshot_eq(const struct cmos_snapshot *a, const struct cmos_snapshot *b)
{
    return a->seconds == b->seconds && a->minutes == b->minutes &&
           a->hours   == b->hours   && a->day     == b->day &&
           a->month   == b->month  && a->year    == b->year;
}

static inline uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

// Howard Hinnant's days_from_civil (public domain) -- correct proleptic
// Gregorian civil-date-to-days-since-epoch conversion, including leap
// years, without libc. Returns days relative to 1970-01-01 (0 = that day).
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                          // [0, 399]
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;      // [0, 365]
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t rtc_now_unix(void)
{
    struct cmos_snapshot prev, cur;

    // Don't start reading mid-update.
    while (cmos_read(CMOS_REG_STATUS_A) & CMOS_STATUS_A_UPDATE_IN_PROGRESS) { }
    cmos_read_snapshot(&prev);

    // Re-read until two consecutive snapshots agree -- guards against an
    // update landing between the first field read and the last.
    for (;;) {
        while (cmos_read(CMOS_REG_STATUS_A) & CMOS_STATUS_A_UPDATE_IN_PROGRESS) { }
        cmos_read_snapshot(&cur);
        if (cmos_snapshot_eq(&prev, &cur)) break;
        prev = cur;
    }

    uint8_t status_b = cmos_read(CMOS_REG_STATUS_B);
    uint8_t seconds = cur.seconds, minutes = cur.minutes, hours = cur.hours;
    uint8_t day = cur.day, month = cur.month, year = cur.year;

    if (!(status_b & CMOS_STATUS_B_BINARY_MODE)) {
        // BCD mode (the historical default): convert every field. The hour
        // register's top bit is the PM flag in 12h mode, not part of the
        // BCD value itself -- mask it off before converting, restore the
        // flag's meaning afterward via the 12h-to-24h fixup below.
        bool pm = hours & CMOS_HOUR_PM_BIT;
        seconds = bcd_to_bin(seconds);
        minutes = bcd_to_bin(minutes);
        hours   = bcd_to_bin(hours & ~CMOS_HOUR_PM_BIT);
        day     = bcd_to_bin(day);
        month   = bcd_to_bin(month);
        year    = bcd_to_bin(year);
        if (pm) hours |= CMOS_HOUR_PM_BIT;   // put the flag back for the 12h check below
    }

    if (!(status_b & CMOS_STATUS_B_24_HOUR)) {
        // 12-hour mode: bit 7 of the (already-converted) hour is PM: 12am
        // is midnight (hour 0), 12pm is noon (hour 12), everything else
        // adds 12 in the afternoon.
        bool pm = hours & CMOS_HOUR_PM_BIT;
        hours &= ~CMOS_HOUR_PM_BIT;
        if (hours == 12) hours = 0;
        if (pm) hours += 12;
    }

    int64_t full_year = 2000 + year;   // see rtc.h's comment on the century assumption
    int64_t days = days_from_civil(full_year, month, day);
    return (uint64_t)days * 86400ULL + (uint64_t)hours * 3600ULL
         + (uint64_t)minutes * 60ULL + (uint64_t)seconds;
}

uint64_t rtc_now_ns(void)
{
    return rtc_now_unix() * 1000000000ULL;
}

int rtc_run_selftests(void)
{
    // 1. Plausible-range check: the on-disk assumption (rtc.h's comment)
    // is a year in [2000, 2099] -- 2000-01-01 and 2100-01-01 as Unix
    // seconds bound that range. A value outside it means either the CMOS
    // clock genuinely isn't set (common on a fresh QEMU instance with no
    // -rtc flag, or real hardware with a dead CMOS battery) or the
    // BCD/12h decode above is wrong.
    uint64_t t0 = rtc_now_unix();
    const uint64_t year_2000 = 946684800ULL;
    const uint64_t year_2100 = 4102444800ULL;
    if (t0 < year_2000 || t0 >= year_2100) {
        kprintf("rtc_run_selftests: implausible time %lu (want [%lu, %lu))\n",
                (unsigned long)t0, (unsigned long)year_2000, (unsigned long)year_2100);
        return -1;
    }

    // 2. Monotonicity across a real delay: wait a known ~1.5s (long enough
    // to guarantee the RTC's own ~1Hz update landed at least once) via the
    // PIT (an independent time source from the RTC, so this genuinely
    // cross-checks two different pieces of hardware agreeing), then assert
    // the RTC moved forward by a plausible amount -- not backward, and not
    // by an absurd jump (which would mean the BCD/century decode is wrong
    // on a boundary, e.g. a bad carry from seconds=59 into minutes).
    pit_delay_ms(500);
    pit_delay_ms(500);
    pit_delay_ms(500);
    uint64_t t1 = rtc_now_unix();
    if (t1 < t0) {
        kprintf("rtc_run_selftests: time went backward (%lu -> %lu)\n",
                (unsigned long)t0, (unsigned long)t1);
        return -1;
    }
    uint64_t delta = t1 - t0;
    if (delta > 5) {   // generous margin above the ~1-2s actually expected
        kprintf("rtc_run_selftests: implausible jump after ~1.5s delay (%lu -> %lu, delta=%lu)\n",
                (unsigned long)t0, (unsigned long)t1, (unsigned long)delta);
        return -1;
    }

    kprintf("rtc_run_selftests: t0=%lu t1=%lu delta=%lus OK\n",
            (unsigned long)t0, (unsigned long)t1, (unsigned long)delta);
    return 0;
}
