# CH32H417 port

MicroPython for the WCH CH32H417 dual-core RISC-V MCU, running on the QingKe
V5F core (`wch_riscv.cpu.1`) at 400 MHz. The V3F is a boot stub that starts it.

## Building

Requires a PlatformIO package store for the toolchain and vendor SDK:

    make -C ../../mpy-cross
    make BOARD=CH32H417QEU6_V5F -j8
    make BOARD=CH32H417QEU6_V5F deploy

Override `PIO_PKGS`, `CROSS_COMPILE` or `SDK` if your packages live elsewhere.
Under Git Bash, pass `PYTHON=/c/path/to/python.exe` — a bare `python` hits the
Windows Store stub.

## Cores

MicroPython runs on the **V5F** (400 MHz, out-of-order, 32 KB I-cache). The
V3F is reduced to a boot stub at flash `0x00000000` that configures the PLLs,
calls `NVIC_WakeUp_V5F(0x00010000)` and then sleeps.

Two images are built:

    boot_v3f/build/boot_v3f.elf           -> flash 0x00000000
    build-CH32H417QEU6_V5F/firmware.elf   -> flash 0x00010000

**They must be merged before flashing.** This chip's OpenOCD flash driver
mass-erases the entire flash on every `program` command, so writing them as
two operations silently destroys the first while still reporting
"Verified OK". `make deploy` merges via `scripts/merge_flash.py` and writes
once.

The V5F image must never call `SystemInit()`: the stub has already configured
every PLL, and re-running it would reconfigure the clock tree underneath a
running core.

`CH32H417QEU6_V3F` is still supported and is the fallback.

## Board

`CH32H417QEU6_V5F` (default) — REPL on USART1, PA9 (TX) / PA10 (RX), 115200 8N1.

## Memory layout

Only ITCM and DTCM are zero-wait at the V5F's 400 MHz core clock. The shared
region runs at HCLK (100 MHz) and code flash is roughly 25 MHz-equivalent, so
instruction placement dominates performance.

    FLASH     0x00000000   64K   V3F boot stub
    FLASH     0x00010000  896K   V5F image
    ITCM      0x200A0000  128K   .itcm_text: all of py/ + shared/runtime/ (92K used)
    RAM_CODE  0x20100000  256K   .highcode: extmod, SDK drivers, port files (42K used)
    DTCM      0x200C0000  256K   .data/.bss/stack/GC heap

`main()` copies `.itcm_text` into ITCM before calling into it; the SDK startup
file only knows about `.highcode`.

## Measured

    text 137964   data 3120   bss 17432     heap ~235 KB
    core clock 400 MHz

    benchmark  254 ms   (V3F baseline 4297 ms -> 16.9x)
    upstream tests: 461 passed / 0 failed / 110 skipped (15210 testcases)

See `docs/hw/benchmarks.md` for the layout comparison.

## Floating point

Built `-march=rv32imafc_zba_zbb_zbc_zbs_xw -mabi=ilp32f`. Both cores report
the same `misa=0x40901127`, including a hardware single-precision FPU, paired with
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
