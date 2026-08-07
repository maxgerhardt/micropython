#include <stdint.h>
// py/ uses alloca() for call frames; without this it is implicitly declared.
#include <alloca.h>

// Board-specific constants (name, UART pins/baud, boot delay).
#include "mpconfigboard.h"

// Feature level: everything the REPL needs, nothing more, for Milestone 1.
#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

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
#define MICROPY_ENABLE_EXTERNAL_IMPORT  (0)
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE   (256)

// GC needs callee-saved registers spilled; we use the RISC-V native helper.
#define MICROPY_GCREGS_SETJMP           (0)

// No native emitter in Milestone 1. Plain RV32IMC codegen can be enabled later.
#define MICROPY_EMIT_RV32               (0)

#define MICROPY_PY_SYS_PLATFORM         "ch32"
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

// No filesystem yet: that arrives with littlefs in Milestone 3. MICROPY_PY_IO
// defaults on at CORE_FEATURES and pulls in mp_builtin_open_obj, which has
// nothing to open, so turn it off too.
#define MICROPY_VFS                     (0)
#define MICROPY_PY_OS                   (0)
#define MICROPY_PY_IO                   (0)
#define MICROPY_READER_VFS              (0)

// Types used by py/ on this target.
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

#define MP_STATE_PORT MP_STATE_VM
