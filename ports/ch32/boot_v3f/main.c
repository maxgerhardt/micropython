/* V3F boot stub for the CH32H417.
 *
 * Its entire job is to configure the clock tree and hand control to the V5F,
 * which runs MicroPython. It then sleeps and never runs again.
 *
 * USART1 output here is deliberately minimal but not absent: without it, a
 * failure to wake the V5F is indistinguishable from a dead board. */
#include "ch32h417.h"
#include "system_ch32h417.h"

#ifndef CH32_V5F_START_ADDR
#error "CH32_V5F_START_ADDR must be defined (see coreboot.mk)"
#endif

static void uart1_init(uint32_t baud) {
    GPIO_InitTypeDef gpio = {0};
    USART_InitTypeDef usart = {0};

    /* AFIO clock is required or the GPIO_PinAFConfig writes are dropped. */
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOA | RCC_HB2Periph_USART1
        | RCC_HB2Periph_AFIO, ENABLE);
    /* The H417 uses an STM32F4-style AF mux: USART1 is AF7 on PA9/PA10. */
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF7);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF7);

    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_High;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_10;
    GPIO_Init(GPIOA, &gpio);

    usart.USART_BaudRate = baud;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &usart);
    USART_Cmd(USART1, ENABLE);
}

static void uart1_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
            }
            USART_SendData(USART1, '\r');
        }
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART1, (uint16_t)(uint8_t)*s++);
    }
}

int main(void) {
    SystemInit();                 /* configures every PLL; the V5F must NOT repeat this */
    SystemAndCoreClockUpdate();
    uart1_init(115200);

    uart1_puts("\nV3F: waking V5F\n");

    /* The address is masked with ~0x3FF inside, so it must be 1 KB aligned. */
    NVIC_WakeUp_V5F(CH32_V5F_START_ADDR);

    RCC_HB1PeriphClockCmd(RCC_HB1Periph_PWR, ENABLE);
    PWR_EnterSTOPMode(PWR_Regulator_ON, PWR_STOPEntry_WFE);

    /* Reached only if STOP mode returns; keep the core harmlessly parked. */
    for (;;) {
        __asm volatile ("nop");
    }
}
