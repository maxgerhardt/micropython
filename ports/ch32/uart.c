#include "ch32h417.h"
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"
#include "uart.h"

/* Polled RX drops bytes whenever the VM is busy between reads, so the console
 * is served from an interrupt-filled ring buffer instead. */
/* The ISR feeds stdin_ringbuf, which the USB CDC layer also fills, so both
 * consoles share one input queue. */
#include "py/ringbuf.h"
#include "mphalport.h"
#include "irq.h"

void uart_init(uint32_t baud) {
    GPIO_InitTypeDef gpio = {0};
    USART_InitTypeDef usart = {0};

    /* AFIO must be clocked: GPIO_PinAFConfig writes land in the AFIO block and
     * are silently dropped while its clock is off. */
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOA | RCC_HB2Periph_USART1
        | RCC_HB2Periph_AFIO, ENABLE);

    /* The H417 has an STM32F4-style alternate-function mux: GPIO_Mode_AF_PP
     * only selects "some alternate function", GPIO_PinAFConfig picks which one.
     * Per datasheet p.74, USART1 is AF7 on PA9/PA10. Omitting this leaves the
     * pad connected to the wrong peripheral and nothing appears on the wire. */
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF7);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF7);

    gpio.GPIO_Pin = GPIO_Pin_9;              /* PA9 = TX */
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_High;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = GPIO_Pin_10;             /* PA10 = RX */
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    usart.USART_BaudRate = baud;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &usart);
    USART_Cmd(USART1, ENABLE);

    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    NVIC_EnableIRQ(USART1_IRQn);
}

void uart_tx_strn(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART1, (uint16_t)(uint8_t)str[i]);
    }
}

bool uart_rx_any(void) {
    return ringbuf_peek(&stdin_ringbuf) != -1;
}

int uart_rx_chr(void) {
    return ringbuf_get(&stdin_ringbuf);
}

void CH32_IRQ_HANDLER(USART1_IRQHandler);
void USART1_IRQHandler(void) {
    if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
        uint8_t c = (uint8_t)(USART_ReceiveData(USART1) & 0xff);
        if (c == mp_interrupt_char) {
            /* Break out of a running script; do not buffer the byte. */
            mp_sched_keyboard_interrupt();
            return;
        }
        ringbuf_put(&stdin_ringbuf, c);   /* drops silently when full */
    }
}
