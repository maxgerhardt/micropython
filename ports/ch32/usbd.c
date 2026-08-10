/* USBFS device support for the CH32H417.
 *
 * The controller is on PA11 (OTG_DM) / PA12 (OTG_DP). PA9 and PA10 are
 * OTG_VBUS and OTG_ID on this package and carry the UART REPL, so neither
 * VBUS sensing nor the ID pin is enabled -- a device-only build needs neither. */
#include "ch32h417.h"

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"

#include "tusb.h"
/* For the mp_usbd_port_get_serial_number() prototype: including it means a
 * signature mismatch is a compile error rather than a silent ABI accident. */
#include "shared/tinyusb/mp_usbd.h"

#include "usbd.h"
#include "irq.h"

#ifndef CH32_USBD_CLOCK_DEBUG
#define CH32_USBD_CLOCK_DEBUG (0)
#endif

/* Bounds of the .usbram section, from the board linker script. */
extern uint8_t _susbram[];
extern uint8_t _eusbram[];

void ch32_usbd_init(void) {
    /* TinyUSB's .bss is relocated into USB_RAM so the controller's DMA can
     * reach it, which puts it outside the _sbss.._ebss range the startup code
     * clears. Nothing else zeroes it, so do it here -- before any TinyUSB code
     * runs. Skipping this leaves every endpoint buffer, transfer descriptor and
     * class-driver state variable holding whatever survived in RAM, which
     * presents as USB working or not depending on the build. */
    memset(_susbram, 0, (size_t)(_eusbram - _susbram));

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
    /* Source the PLL from the 25 MHz crystal, not the internal RC. Full-speed
     * USB requires a 0.25%-accurate clock; an on-chip RC oscillator is roughly
     * an order of magnitude worse than that, so an HSI-derived USB clock only
     * appears to work -- the SIE detects bus reset and suspend, which are DC
     * conditions needing no clock, but never decodes a single packet. The board
     * runs from HSE as well (see SYSCLK_..._HSE in mpconfigboard.mk), so the
     * crystal is already running by the time this is reached.
     *
     * The USBHS PLL is off after reset (RCC_CTLR bit 20, RCC_USBHS_PLLON), so
     * it must be configured and started before USBFS can select it.
     *
     * Do NOT call RCC_HSEConfig() here to "make sure" the crystal is on: it
     * clears HSEON before setting it, and with the whole clock tree now derived
     * from HSE that momentarily removes the reference from under the running
     * system. The USBHS PLL does not recover from it and never reports lock. */
    if (!(RCC->CTLR & RCC_HSERDY)) {
        /* Booted on the internal RC after all; USB cannot meet spec from it, so
         * leave USB down rather than enumerating a device that half works. */
        return;
    }

    RCC_USBHSPLLCLKConfig(RCC_USBHSPLLSource_HSE);
    RCC_USBHSPLLReferConfig(RCC_USBHSPLLRefer_25M);
    RCC_USBHS_PLLCmd(ENABLE);

    /* Wait for lock rather than spinning a fixed count: the USBFS clock mux
     * below does not latch while its source PLL is stopped. */
    for (volatile uint32_t i = 0; i < 2000000; i++) {
        if (RCC->CTLR & RCC_USBHS_PLLRDY) {
            break;
        }
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
        while (*lbl) {
            b[n++] = *lbl++;
        }
        for (int i = 7; i >= 0; i--) {
            b[n++] = hx[(ctlr >> (i * 4)) & 0xf];
        }
        lbl = " CFGR2=";
        while (*lbl) {
            b[n++] = *lbl++;
        }
        for (int i = 7; i >= 0; i--) {
            b[n++] = hx[(cfgr2 >> (i * 4)) & 0xf];
        }
        b[n++] = '\r';
        b[n++] = '\n';
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

    tusb_init(0);
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

void CH32_IRQ_HANDLER(USBFS_IRQHandler);
void USBFS_IRQHandler(void) {
    ch32_usbd_irq_count++;
    tud_int_handler(0);
}

/* TinyUSB asks the port for the serial number string. The chip's 96-bit unique
 * ID is at 0x1FFFF7E8, the same location machine.unique_id() reads.
 *
 * The buffer is an uninitialised stack array and the caller walks it looking
 * for a NUL, up to MICROPY_HW_USB_DESC_STR_MAX. Terminating it is therefore not
 * optional: without the NUL the serial string picks up whatever stack garbage
 * follows, and the resulting string descriptor is malformed in a way that
 * depends on the caller's stack contents. That fails enumeration *after* the
 * device descriptor has been read successfully, and only for some builds -- an
 * unpleasant thing to chase, so it is spelled out here. */
void mp_usbd_port_get_serial_number(char *buf) {
    const uint8_t *id = (const uint8_t *)0x1FFFF7E8;
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 12; i++) {
        buf[i * 2] = hex[id[i] >> 4];
        buf[i * 2 + 1] = hex[id[i] & 0xf];
    }
    buf[24] = '\0';
}
