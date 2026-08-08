#include <stdint.h>
// py/ uses alloca() for call frames; without this it is implicitly declared.
#include <alloca.h>

// Board-specific constants (name, UART pins/baud, boot delay).
#include "mpconfigboard.h"

// Feature level: everything the REPL needs, nothing more, for Milestone 1.
#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

// f-strings default to EXTRA_FEATURES, but they are ordinary Python syntax now
// rather than a luxury: micropython-lib's unittest uses them, and the upstream
// test suite requires unittest, so without this a large part of tests/ cannot
// run at all. Enabled on its own rather than raising the whole ROM level.
#define MICROPY_PY_FSTRINGS             (1)

// Likewise memoryview and slice assignment. Both also default to
// EXTRA_FEATURES, and without them the upstream suite fails tests that it does
// not know to skip (extmod/vfs_fat_ramdisklarge, misc/non_compliant,
// micropython/heapalloc_slice). memoryview in particular is what lets code
// pass a window onto a buffer around without copying, which is the normal way
// to do zero-copy I/O on a board with this little RAM.
#define MICROPY_PY_BUILTINS_MEMORYVIEW  (1)
#define MICROPY_PY_ARRAY_SLICE_ASSIGN   (1)
#define MICROPY_PY_BUILTINS_BYTES_HEX   (1)

#define MICROPY_ALLOC_PATH_MAX          (256)
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_ENABLE_PYSTACK          (0)
#define MICROPY_STACK_CHECK             (1)
#define MICROPY_ENABLE_FINALISER        (1)
#define MICROPY_HELPER_REPL             (1)
#define MICROPY_REPL_AUTO_INDENT        (1)
#define MICROPY_REPL_EVENT_DRIVEN       (0)
#define MICROPY_KBD_EXCEPTION           (1)
#define MICROPY_ENABLE_SOURCE_LINE      (1)

#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE   (256)

// GC needs callee-saved registers spilled; we use the RISC-V native helper.
#define MICROPY_GCREGS_SETJMP           (0)

// No native emitter in Milestone 1. Plain RV32IMC codegen can be enabled later.
#define MICROPY_EMIT_RV32               (0)

#define MICROPY_PY_SYS_PLATFORM         "ch32"
// Defaults to EXTRA_FEATURES only, but the upstream test suite expects it.
#define MICROPY_PY_SYS_MAXSIZE          (1)
#define MICROPY_PY_BUILTINS_HELP        (1)
#define MICROPY_PY_BUILTINS_HELP_MODULES (1)

#define MICROPY_PY_MACHINE              (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE  "ports/ch32/modmachine.c"
#define MICROPY_PY_MACHINE_BARE_METAL_FUNCS (1)
#define MICROPY_PY_MACHINE_RESET        (1)
#define MICROPY_PY_MACHINE_MEMX         (1)
#define MICROPY_PY_MACHINE_PULSE        (0)

// time.sleep/ticks_* ride on the SysTick HAL via extmod's default bodies,
// which call mp_hal_delay_ms/us and mp_hal_ticks_ms/us/cpu directly.
#define MICROPY_PY_TIME                 (1)
#define MICROPY_PY_TIME_TICKS           (1)

// USB device: CDC console on the USBFS controller (PA11/PA12).
/* The USB stack defers work with mp_sched_schedule_node, which needs this. */
#define MICROPY_ENABLE_SCHEDULER        (1)
#define MICROPY_SCHEDULER_STATIC_NODES  (1)
#define MICROPY_HW_ENABLE_USBDEV        (1)
#define MICROPY_HW_USB_CDC              (1)
#define MICROPY_HW_USB_MSC              (1)
#define MICROPY_HW_USB_MSC_INQUIRY_VENDOR_STRING  "WCH"
#define MICROPY_HW_USB_MSC_INQUIRY_PRODUCT_STRING "CH32H417 Flash"
#define MICROPY_HW_USB_VID              (0x1209)
#define MICROPY_HW_USB_PID              (0x0001)
#define MICROPY_HW_USB_MANUFACTURER_STRING "WCH"
#define MICROPY_HW_USB_PRODUCT_FS_STRING   "CH32H417 MicroPython"

/* The USB stack has to be serviced from two places, and both are needed.
 *
 * MICROPY_INTERNAL_EVENT_HOOK runs from mp_event_handle_nowait(), which is what
 * the REPL spins on while waiting for input -- without it, a board sitting at
 * the prompt never processes USB at all.
 *
 * The VM hooks cover the other case: a long-running Python loop never reaches
 * the event hook, and the host would eventually drop the connection. */
void mp_hal_ch32_poll_usb(void);
#define MICROPY_INTERNAL_EVENT_HOOK mp_hal_ch32_poll_usb()
/* The ISR does the time-critical USB work; tud_task() only drains deferred
 * events, so polling it every 16 bytecodes cost ~18%% on the benchmark for no
 * benefit. 256 keeps the host happy and the overhead unmeasurable. */
#define MICROPY_VM_HOOK_COUNT (256)
#define MICROPY_VM_HOOK_INIT static uint vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL if (--vm_hook_divisor == 0) {         vm_hook_divisor = MICROPY_VM_HOOK_COUNT;                   mp_hal_ch32_poll_usb();                                }
#define MICROPY_VM_HOOK_LOOP MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN MICROPY_VM_HOOK_POLL

// Filesystem: FAT on the internal flash tail. FAT rather than littlefs because
// the volume is exposed over USB MSC later and hosts cannot read littlefs.
#define MICROPY_VFS                     (1)
#define MICROPY_PY_OS                   (1)
/* Flushes the block device page cache; the port keeps up to 8 KB dirty. */
#define MICROPY_PY_OS_SYNC              (1)
#define MICROPY_PY_IO                   (1)
/* print(..., file=f) needs both this and MICROPY_PY_IO; sys_stdio_mphal.c is
 * already in the build to provide sys.stdin/stdout/stderr. */
#define MICROPY_PY_SYS_STDFILES         (1)
#define MICROPY_READER_VFS              (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT  (1)
/* Import precompiled .mpy files. Without this the importer only ever looks for
 * .py, so a board cannot run code cross-compiled with mpy-cross -- and the
 * upstream suite's extmod/vfs_userfs test, which imports a deliberately
 * malformed .mpy to check the file is closed on error, fails outright. */
#define MICROPY_PERSISTENT_CODE_LOAD    (1)
#define MICROPY_FATFS_ENABLE_LFN        (1)
/* Token-pasted into a table name by ffunicode.c, so it must be a bare
 * number: parentheses here produce "pasting uc and ( is invalid". */
#define MICROPY_FATFS_LFN_CODE_PAGE     437
#define MICROPY_FATFS_MAX_SS            (512)
#define MICROPY_FATFS_RPATH             (2)

// Types used by py/ on this target.
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

// No SSIZE_MAX in this newlib's headers; sys.maxsize needs it.
#define MP_SSIZE_MAX (0x7fffffff)

#define MP_STATE_PORT MP_STATE_VM
