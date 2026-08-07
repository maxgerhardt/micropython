#ifndef MICROPY_INCLUDED_CH32_UART_H
#define MICROPY_INCLUDED_CH32_UART_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void uart_init(uint32_t baud);
void uart_tx_strn(const char *str, size_t len);
bool uart_rx_any(void);
int uart_rx_chr(void);   // returns -1 when no byte is available

#endif // MICROPY_INCLUDED_CH32_UART_H
