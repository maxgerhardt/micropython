/* USBFS device support for the CH32H417.
 *
 * The controller is on PA11 (OTG_DM) / PA12 (OTG_DP). PA9 and PA10 are
 * OTG_VBUS and OTG_ID on this package and carry the UART REPL, so neither
 * VBUS sensing nor the ID pin is enabled -- a device-only build needs neither. */
#include "ch32h417.h"

#include "py/mphal.h"
#include "py/runtime.h"

#include "tusb.h"

#include "usbd.h"

#ifndef CH32_USBD_CLOCK_DEBUG
#define CH32_USBD_CLOCK_DEBUG (0)
#endif

void ch32_usbd_init(void) {
    /* USBFS needs exactly 48 MHz.
     *
     * The system PLL cannot produce it: SYSCLK is 400 MHz and the USBFS
     * dividers are 1,2,3,4,5,6,8,10 plus half-steps, so 400/8.33 is not
     * reachable. The USBHS PLL runs at 480 MHz, and 480/10 is exactly 48, so
     * that is the source used here.
     *
     * A wrong USB clock presents as a device that never enumerates rather than
     * as any kind of error, so if enumeration fails this is the first thing to
     * check -- read RCC->CFGR2 back over SWD. */
    /* The USBHS PLL is off after reset (RCC_CTLR bit 20, RCC_USBHS_PLLON), so
     * it must be configured and started before USBFS can select it. */
    RCC_USBHSPLLCLKConfig(RCC_USBHSPLLSource_HSI);
    RCC_USBHSPLLReferConfig(RCC_USBHSPLLRefer_25M);
    RCC_USBHS_PLLCmd(ENABLE);
    for (volatile uint32_t i = 0; i < 200000; i++) {
    }

    RCC_USBFSCLKConfig(RCC_USBFSCLKSource_USBHSPLL);
    RCC_USBFS48ClockSourceDivConfig(RCC_USBFS_Div10);

    #if CH32_USBD_CLOCK_DEBUG
    {
        extern void uart_tx_strn(const char *str, size_t len);
        char b[64];
        int n = 0;
        const uint32_t ctlr = RCC->CTLR, cfgr2 = RCC->CFGR2;
        static const char hx[] = "0123456789abcdef";
        const char *lbl = "\r\nUSBCLK CTLR=";
        while (*lbl) { b[n++] = *lbl++; }
        for (int i = 7; i >= 0; i--) { b[n++] = hx[(ctlr >> (i * 4)) & 0xf]; }
        lbl = " CFGR2=";
        while (*lbl) { b[n++] = *lbl++; }
        for (int i = 7; i >= 0; i--) { b[n++] = hx[(cfgr2 >> (i * 4)) & 0xf]; }
        b[n++] = '\r'; b[n++] = '\n';
        uart_tx_strn(b, n);
    }
    #endif

    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOA | RCC_HB2Periph_AFIO, ENABLE);

    /* PA11/PA12 carry the USB differential pair. The H417 uses an
     * STM32F4-style AF mux, so GPIO_Mode_AF_PP alone is not enough -- but the
     * USB pads are driven by the controller directly rather than through a
     * numbered alternate function, so no GPIO_PinAFConfig is needed here. */
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_11 | GPIO_Pin_12;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(GPIOA, &gpio);

    RCC_HBPeriphClockCmd(RCC_HBPeriph_OTG_FS, ENABLE);

    tud_init(0);
    NVIC_EnableIRQ(USBFS_IRQn);
}

/* Diagnostics, queryable from the REPL as ch32.usb_stat(). USB faults on this
 * chip are otherwise indistinguishable from one another. */
volatile uint32_t ch32_usbd_task_count;
volatile uint32_t ch32_usbd_irq_count;

void ch32_usbd_task(void) {
    ch32_usbd_task_count++;
    tud_task();
}

void USBFS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USBFS_IRQHandler(void) {
    ch32_usbd_irq_count++;
    tud_int_handler(0);
}

/* TinyUSB asks the port for the serial number string. The chip's 96-bit unique
 * ID is at 0x1FFFF7E8, the same location machine.unique_id() reads. */
size_t mp_usbd_port_get_serial_number(uint8_t *buf) {
    const uint8_t *id = (const uint8_t *)0x1FFFF7E8;
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 12; i++) {
        buf[i * 2] = hex[id[i] >> 4];
        buf[i * 2 + 1] = hex[id[i] & 0xf];
    }
    return 24;
}
