#ifndef MICROPY_INCLUDED_CH32_IRQ_H
#define MICROPY_INCLUDED_CH32_IRQ_H

/* Interrupt handler attribute.
 *
 * WCH's GCC takes an argument selecting its fast interrupt entry sequence,
 * which uses the custom `xw` instructions and the hardware stack push. A stock
 * RISC-V GCC has never heard of that string and rejects it outright, so a build
 * with CH32_TOOLCHAIN=generic falls back to the standard attribute: a slower
 * prologue that saves registers the ordinary way, but one that compiles.
 *
 * Declare handlers as:
 *
 *     void CH32_IRQ_HANDLER(SysTick0_Handler);
 *     void SysTick0_Handler(void) { ... }
 *
 * The declaration carries the attribute and the definition does not, which is
 * what GCC wants -- an attribute on the definition alone is applied too late to
 * change the prologue it has already emitted.
 */
#if CH32_TOOLCHAIN_WCH
#define CH32_IRQ_HANDLER(name) name(void) __attribute__((interrupt("WCH-Interrupt-fast")))
#else
#define CH32_IRQ_HANDLER(name) name(void) __attribute__((interrupt))
#endif

#endif // MICROPY_INCLUDED_CH32_IRQ_H
