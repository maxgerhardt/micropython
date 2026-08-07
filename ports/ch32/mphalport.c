#include "ch32h417.h"
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"

#include "uart.h"
#include "mphalport.h"

void mp_hal_init(void) {
    // Filled in with the SysTick time base in the next task.
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    uart_tx_strn(str, len);
    return len;
}

int mp_hal_stdin_rx_chr(void) {
    for (;;) {
        int c = uart_rx_chr();
        if (c >= 0) {
            return c;
        }
        mp_event_handle_nowait();
    }
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    uintptr_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) && uart_rx_any()) {
        ret |= MP_STREAM_POLL_RD;
    }
    if (poll_flags & MP_STREAM_POLL_WR) {
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}
