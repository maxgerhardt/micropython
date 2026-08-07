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
    ITCM      0x200A0000  128K   .itcm_text: all of py/ + shared/runtime/ (96K used)
    RAM_CODE  0x20100000  256K   .highcode: extmod, oofatfs, SDK drivers, port files (64K used)
    DTCM      0x200C0000  256K   .data/.bss/stack/GC heap

`main()` copies `.itcm_text` into ITCM before calling into it; the SDK startup
file only knows about `.highcode`.

## Filesystem

A 512 KB FAT volume occupies the flash tail at `0x08070000`, mounted at `/`.
A blank board formats itself on first boot. `boot.py` and `main.py` run at
startup if present; an exception in either is reported and the REPL still
comes up.

    0x08000000    64K   V3F boot stub
    0x08010000   384K   V5F MicroPython image (163 KB used)
    0x08070000   512K   FAT volume
    0x080F0000          end of the 960 KB user area

The erase unit is 8 KB while FAT writes 512-byte sectors, so writes go through
a single 8 KB write-back page cache; `os.sync()` flushes it. The block device
is also exposed as `ch32.Flash()`.

**FAT on raw flash is not power-loss safe.** An interruption between erase and
reprogram loses the affected 8 KB page. FAT was chosen over littlefs because
the volume is exposed over USB MSC later and hosts cannot read littlefs.

**Reflashing firmware erases the filesystem.** This chip's flash driver
mass-erases on every program command, so `make deploy` wipes user files.

Note that OpenOCD reports `flash size = 512kbytes`; that is an assumption in
its `wch_riscv` driver, not a hardware limit. The part is 960 KB
(`FLASH_CFGR0` bit 28 set) and firmware writes the upper region directly,
verified by erasing and programming at `0x080EE000`.

File timestamps are a fixed date: nothing sets the RTC yet. The plan is to
sync it over NTP once Ethernet lands.

## USB

A USB CDC REPL runs on the **USBFS** controller, PA11 (OTG_DM) / PA12 (OTG_DP),
alongside the UART one. Output goes to both consoles and input is accepted from
either, so a USB problem never costs you the debug console. VID/PID are
0x1209/0x0001 (pid.codes test IDs).

USBFS does **not** share pins with the SWD debug interface -- USBHS does
(PB8/PB9) -- so USB and debugging work together.

PA9 and PA10 are OTG_VBUS and OTG_ID on this package and carry the UART REPL,
so device-only USB leaves VBUS sensing and the ID pin unused.

The 512 KB FAT volume is also exposed as a **USB mass-storage drive**, so the
board enumerates as a composite CDC + MSC device.

**There is no arbitration between the host and MicroPython.** Both may write,
and a host that has cached FAT directory structures will overwrite changes it
did not see -- a file written from MicroPython while the drive is mounted can
simply vanish when the host next writes. This matches `ports/stm32` and
`ports/rp2`; CircuitPython is the one that makes the volume exclusive to one
side. In practice: do not write from both sides at once, and `os.sync()` and
eject before switching.

Expect the host to write to the volume unprompted -- Windows creates
`System Volume Information`, and may add indexing and recycle-bin data. That is
normal for any MSC device, not corruption.

CH32H417 support for TinyUSB lives in a fork, `maxgerhardt/tinyusb` branch
`ch32h417`, wired in as `lib/tinyusb`. Cloning therefore needs `--recursive`.

Two things that cost real time and are worth knowing:

- **The USBFS clock bits in `RCC_CFGR2` do not latch while the PLL they select
  is stopped.** USBFS needs exactly 48 MHz and cannot get there from the 400 MHz
  system PLL (dividers are 1,2,3,4,5,6,8,10 plus half-steps; 400/8.33 is not
  reachable), so it sources the 480 MHz USBHS PLL divided by 10. That PLL must
  be started *first* via `RCC_USBHS_PLLCmd()`, or the `CFGR2` writes silently
  evaporate and the device never enumerates.
- **The SDK's `USBFSD_TypeDef` is not layout-compatible with TinyUSB's endpoint
  indexing**, despite identical field names. `UEPn_TX_LEN` is `uint8_t` where the
  CH32V307's is `uint16_t`, halving the driver's endpoint stride, and `UEP3`
  omits its `RESERVED` byte. The fork declares its own struct with static
  asserts on the offsets.

## Measured

    text 176864   data 3524   bss 30596     heap ~228 KB
    core clock 400 MHz

    benchmark  322 ms   (V3F baseline 4297 ms -> 13.3x)
    upstream tests: 470 passed / 0 failed (15270 testcases)

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

Ethernet, and the RV32 native emitter (which can be enabled
later targeting plain RV32IMC — the core is a superset, so no `xw` support is
needed in the emitter).
