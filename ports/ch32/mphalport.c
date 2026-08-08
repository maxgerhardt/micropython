#include "ch32h417.h"
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"

#ifndef MICROPY_HW_STDIN_BUFFER_LEN
#define MICROPY_HW_STDIN_BUFFER_LEN 512
#endif
static uint8_t stdin_ringbuf_array[MICROPY_HW_STDIN_BUFFER_LEN];
ringbuf_t stdin_ringbuf = { stdin_ringbuf_array, sizeof(stdin_ringbuf_array) };

#include "uart.h"
#include "mphalport.h"

#if MICROPY_HW_ENABLE_USBDEV
#include "shared/tinyusb/mp_usbd_cdc.h"
#include "usbd.h"
#endif

/* SysTick0 is the V3F core's counter (SysTick1 belongs to the V5F).
 * STK_CTLR_0 bits, per reference manual 4.6.1.1:
 *   0 EN_0           enable
 *   1 IE_0           interrupt enable
 *   2 NO_RTC_0       1 = HCLK clock source, 0 = HCLK/8
 *   3 AUTO_RELOAD_0  reload from CMP on match
 *   4 DOWN_MODE_0    0 = count up
 * CNT is only 32 bits, which wraps every ~43 s at 100 MHz, so a 1 ms
 * auto-reload interrupt extends it into a 64-bit millisecond counter. */
#define STK_EN          (1u << 0)
#define STK_IE          (1u << 1)
#define STK_NO_RTC      (1u << 2)
#define STK_AUTO_RELOAD (1u << 3)

static volatile uint64_t systick_ms;
static uint32_t systick_per_us;  // SysTick counts per microsecond

void SysTick0_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void SysTick0_Handler(void) {
    SysTick0->ISR &= ~(1u << 0);
    systick_ms++;
}

void mp_hal_init(void) {
    /* SysTick counts at HCLK, and HCLK is NOT SystemCoreClock on this part.
     * SystemCoreClock is the V5F core clock, 400 MHz; HCLK is SYSCLK >> 2,
     * 100 MHz. Configuring the counter from the core clock made every tick
     * worth four times what the rest of the code assumed, so the entire
     * timebase ran 4x slow: time.sleep_ms(2000) took eight seconds against a
     * host stopwatch, and anything measuring microseconds -- time_pulse_us(),
     * and so every bit-banged protocol -- read four times short. A DHT11 came
     * back as 40 zero bits because a 70 us high measured as 17 us and fell
     * under the 48 us threshold that separates a one from a zero.
     *
     * Ask RCC rather than assuming, so this cannot drift out of step with a
     * future clock-tree change. */
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    uint32_t systick_hz = clocks.HCLK_Frequency;

    systick_per_us = systick_hz / 1000000u;
    systick_ms = 0;

    SysTick0->CTLR = 0;
    SysTick0->ISR &= ~(1u << 0);
    SysTick0->CNT = 0;
    SysTick0->CMP = (systick_hz / 1000u) - 1u;   // 1 ms period
    SysTick0->CTLR = STK_EN | STK_IE | STK_NO_RTC | STK_AUTO_RELOAD;
    NVIC_EnableIRQ(SysTick0_IRQn);
}

// Read the millisecond counter and the sub-millisecond remainder atomically
// with respect to the SysTick interrupt: if the ms counter moved while we were
// reading CNT, take the sample again.
/* Fold completed-but-unserviced ticks into the millisecond counter.
 *
 * This exists so the timebase does not depend on the interrupt having been
 * taken. Inside a critical section the handler cannot run, CNT keeps wrapping,
 * and without this the microsecond clock would stall -- so mp_hal_delay_us()
 * would never reach its deadline and time_pulse_us() would misread every
 * pulse. A DHT11 frame masks interrupts for about 4 ms, four wraps.
 *
 * The subtlety, and a bug this port shipped: clearing the overflow flag does
 * NOT cancel the interrupt it already requested. The flag asserting is what
 * makes NVIC latch "pending", and it latches whether or not interrupts are
 * currently masked. Clearing the flag afterwards leaves that latched bit
 * alone, so the handler still runs and counts the same tick a second time.
 *
 * That double-count scaled with how hard the clock was polled, since the race
 * is whether this function reaches the flag before the handler does. A Python
 * loop calling ticks_us() every ~20 us ran 3% fast, which reads as jitter; a C
 * loop polling every ~0.1 us -- mp_hal_delay_us(), time_pulse_us(), the SPI
 * transfer loop -- won nearly every time and ran up to 25% fast.
 * time.sleep_us(5000) returned after 4060 us.
 *
 * The fix is to cancel the pending interrupt as well, so exactly one of this
 * function and the handler ever accounts for a given tick. Both halves are
 * needed: an earlier attempt at a purely read-only reconstruction fixed the
 * double-count but could only ever account for one outstanding tick, which
 * stalled the clock inside any critical section longer than a millisecond and
 * stopped DHT decoding entirely.
 *
 * This recovers at most one tick per call, because the overflow flag is a
 * single bit -- a second wrap while it is already set is not queued anywhere.
 * That is enough because callers poll far more often than once per
 * millisecond: a 20 ms window with interrupts masked, spinning on
 * mp_hal_ticks_us(), measures 20008 us. A masked stretch that never polls at
 * all still loses everything past the first tick, and no arrangement of this
 * function can change that. */
static void systick_catch_up(void) {
    uint32_t state = mp_hal_atomic_enter();
    if (SysTick0->ISR & (1u << 0)) {
        SysTick0->ISR &= ~(1u << 0);
        /* Cancel the interrupt this tick already requested, or the handler
         * counts it again as soon as interrupts come back on. */
        NVIC_ClearPendingIRQ(SysTick0_IRQn);
        systick_ms++;
    }
    mp_hal_atomic_exit(state);
}

static uint64_t ticks_us64(void) {
    systick_catch_up();
    uint64_t ms;
    uint32_t cnt;
    do {
        ms = systick_ms;
        cnt = SysTick0->CNT;
    } while (ms != systick_ms);
    return ms * 1000ull + (cnt / systick_per_us);
}

mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)systick_ms;
}

mp_uint_t mp_hal_ticks_us(void) {
    return (mp_uint_t)ticks_us64();
}

mp_uint_t mp_hal_ticks_cpu(void) {
    return (mp_uint_t)SysTick0->CNT;
}

uint64_t mp_hal_time_ns(void) {
    return ticks_us64() * 1000ull;
}

void mp_hal_delay_us(mp_uint_t us) {
    uint64_t deadline = ticks_us64() + us;
    while (ticks_us64() < deadline) {
    }
}

void mp_hal_delay_ms(mp_uint_t ms) {
    uint64_t deadline = systick_ms + ms;
    /* Handle events at least once, even for a zero delay. time.sleep(0) is the
     * documented way to yield to the scheduler, and with a plain while loop the
     * condition is already false on entry, so a scheduled callback would not
     * run until something else happened to poll -- by which time the code that
     * yielded has moved past the point where it expected the callback. */
    mp_event_handle_nowait();
    while (systick_ms < deadline) {
        /* Sleeps the core between ticks instead of spinning on the counter. */
        mp_event_wait_ms((mp_uint_t)(deadline - systick_ms));
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    /* Both consoles receive everything. The UART is always available, so a USB
     * failure never costs the debug console. */
    uart_tx_strn(str, len);

    #if MICROPY_HW_ENABLE_USBDEV
    /* Drops the data when no host is attached rather than blocking: a board
     * with nothing plugged into USB must not stall in print(). */
    mp_usbd_cdc_tx_strn(str, len);
    #endif

    return len;
}

int mp_hal_stdin_rx_chr(void) {
    for (;;) {
        int c = ringbuf_get(&stdin_ringbuf);
        if (c != -1) {
            return c;
        }
        /* This is where a board spends nearly all of its time. Waiting rather
         * than polling lets the core clock gate between interrupts; both
         * consoles that fill the ring buffer are interrupt-driven, so there is
         * nothing to poll for anyway. */
        mp_event_wait_indefinite();
    }
}

/* MICROPY_INTERNAL_WFE. Plain WFI, not the SDK's STOP-mode helper: __WFI()
 * clears the deep-sleep select bit first, so this gates the core clock without
 * asking the SoC to stop anything the other core is still using. */
void mp_hal_ch32_wfe(void) {
    __WFI();
}

#define MSTATUS_MIE (1u << 3)

/* Both of these need Machine mode, which is why the port ships its own copy of
 * startup_ch32h417_v5f.S -- WCH's stock one drops to User mode, where reading
 * mstatus is an illegal instruction. */
uint32_t mp_hal_atomic_enter(void) {
    uint32_t state = __get_MSTATUS();
    __disable_irq();
    return state;
}

void mp_hal_atomic_exit(uint32_t state) {
    /* Put MIE back rather than writing the whole word: an interrupt may
     * legitimately have changed other mstatus bits -- the FPU dirty flag, for
     * one -- and restoring a stale copy would undo that. */
    if (state & MSTATUS_MIE) {
        __enable_irq();
    }
}

/* Timing guard for bit-banged drivers (dht, onewire): a plain interrupt lock.
 *
 * That is only safe because ticks_us64() folds SysTick's hardware overflow
 * flag into its result instead of trusting the handler to have run. Without
 * that, masking SysTick would freeze the millisecond half of the timebase
 * while the counter kept wrapping underneath, so the microsecond clock would
 * sawtooth once per millisecond. */
uint32_t mp_hal_quiet_timing_enter(void) {
    return mp_hal_atomic_enter();
}

void mp_hal_quiet_timing_exit(uint32_t state) {
    mp_hal_atomic_exit(state);
}

#if MICROPY_HW_ENABLE_USBDEV
/* Pumped from the VM hook so the stack keeps running during a long-lived
 * Python loop; without it the host eventually drops the connection. */
void mp_hal_ch32_poll_usb(void) {
    ch32_usbd_task();
}
#endif

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    uintptr_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) && ringbuf_peek(&stdin_ringbuf) != -1) {
        ret |= MP_STREAM_POLL_RD;
    }
    if (poll_flags & MP_STREAM_POLL_WR) {
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}
