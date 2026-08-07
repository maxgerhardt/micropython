# CH32H417 port

MicroPython for the WCH CH32H417 dual-core RISC-V MCU, running on the QingKe
V3F core (`wch_riscv.cpu.0`). The V5F core is not started.

## Building

Requires a PlatformIO package store for the toolchain and vendor SDK:

    make -C ../../mpy-cross
    make BOARD=CH32H417QEU6_V3F -j8
    make BOARD=CH32H417QEU6_V3F deploy

Override `PIO_PKGS`, `CROSS_COMPILE` or `SDK` if your packages live elsewhere.
Under Git Bash, pass `PYTHON=/c/path/to/python.exe` — a bare `python` hits the
Windows Store stub.

## Board

`CH32H417QEU6_V3F` — REPL on USART1, PA9 (TX) / PA10 (RX), 115200 8N1.

## Memory layout

All code is copied from flash into the zero-wait shared SRAM area at boot,
because the V3F has no instruction cache and code flash runs at roughly
25 MHz-equivalent.

    FLASH     0x00000000  960K   image LMA
    RAM_CODE  0x20100000  256K   .text/.rodata (zero-wait)
    RAM_LOAD  0x20140000  256B   flash->RAM copy stub
    RAM       0x20140100  256K   .data/.bss/heap/stack (zero-wait)

The 384 KB of ITCM+DTCM at `0x200A0000` is currently unused; it is intended as
a second GC heap region via `MICROPY_GC_SPLIT_HEAP`.

## Measured

    text 139040   data 3128   bss 17424
    GC heap 241336 bytes, ~235 KB free at a fresh prompt
    V3F core clock 100 MHz (SYSCLK 400 MHz)

    upstream tests: 461 passed / 0 failed / 110 skipped (15210 testcases)

## Floating point

Built `-march=rv32imafc_zba_zbb_zbc_zbs_xw -mabi=ilp32f`. The V3F has a
hardware single-precision FPU (`misa=0x40901127`), paired with
`MICROPY_FLOAT_IMPL_FLOAT`. This differs from the soft-float `ilp32` in the
stock PlatformIO board definition, and is safe because the vendor SDK is
compiled from source with the same flags.

## Port gotchas

- **Alternate functions.** The H417 uses an STM32F4-style AF mux, unlike other
  CH32 parts. `GPIO_Mode_AF_PP` is not sufficient — `GPIO_PinAFConfig()` must
  select the AF number (USART1 is AF7 on PA9/PA10), and the AFIO peripheral
  clock must be enabled or those writes are silently dropped.
- **SysTick.** `SysTick0` belongs to the V3F, `SysTick1` to the V5F. `CNT` is a
  single 32-bit register that wraps every ~43 s at 100 MHz, so a 1 ms
  auto-reload interrupt extends it to 64 bits.
- **Debug pins** are PB8 (SWCLK) / PB9 (SWDIO), shared with USBHS_DP/DM. This
  will need care when USB HS is brought up.
- **`Debug/ch32h417/debug.c` must not be compiled in** — its `_write` retarget
  collides with MicroPython stdio. Its header is still needed on the include
  path because `ch32h417_conf.h` includes it.
- `MICROPY_HW_BOOT_DELAY_LOOPS` holds off clock and peripheral setup at reset,
  leaving a window for a debugger to attach if firmware ever wedges SDI.

## Not yet implemented

Filesystem, USB, Ethernet, and the RV32 native emitter (which can be enabled
later targeting plain RV32IMC — the core is a superset, so no `xw` support is
needed in the emitter).
