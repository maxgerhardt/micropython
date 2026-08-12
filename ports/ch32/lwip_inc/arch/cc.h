#ifndef MICROPY_INCLUDED_CH32_LWIP_ARCH_CC_H
#define MICROPY_INCLUDED_CH32_LWIP_ARCH_CC_H

#include <assert.h>

/* Diagnostics are compiled out. lwip's debug output goes through printf, which
 * on this port writes to the REPL -- so an assert firing inside the stack, in
 * interrupt context, would try to push a string down the same console the user
 * is typing at. Silence is the safer default; turn this into mp_printf when
 * chasing a stack bug. */
#define LWIP_PLATFORM_DIAG(x)
#define LWIP_PLATFORM_ASSERT(x)  { assert(1); }

#define LWIP_NO_CTYPE_H 1

#endif // MICROPY_INCLUDED_CH32_LWIP_ARCH_CC_H
