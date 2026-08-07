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
    // SysTick runs at HCLK (the V3F core clock).
    systick_per_us = SystemCoreClock / 1000000u;
    systick_ms = 0;

    SysTick0->CTLR = 0;
    SysTick0->ISR &= ~(1u << 0);
    SysTick0->CNT = 0;
    SysTick0->CMP = (SystemCoreClock / 1000u) - 1u;   // 1 ms period
    SysTick0->CTLR = STK_EN | STK_IE | STK_NO_RTC | STK_AUTO_RELOAD;
    NVIC_EnableIRQ(SysTick0_IRQn);
}

// Read the millisecond counter and the sub-millisecond remainder atomically
// with respect to the SysTick interrupt: if the ms counter moved while we were
// reading CNT, take the sample again.
static uint64_t ticks_us64(void) {
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
    while (systick_ms < deadline) {
        mp_event_handle_nowait();
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
        mp_event_handle_nowait();
    }
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
