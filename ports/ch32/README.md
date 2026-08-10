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

## GPIO

    from machine import Pin
    p = Pin("PB0", Pin.OUT)         # also Pin(16) or Pin.cpu.PB0
    p.on(); p.off(); p.toggle()
    q = Pin("PB1", Pin.IN, Pin.PULL_UP)
    q.irq(lambda pin: print(pin), Pin.IRQ_RISING)

All 95 I/O ports are exposed: PA0-PA15, PB0-PB15, PC0-PC15, PD0-PD15,
PE0-PE15 and PF0-PF14. There is no PF15. The QEU6 is the QFN128 package, the
largest of the family, and bonds out every pin the die has — which is why the
SDK's `GPIO_IPD_Unused()` has no case for this part. A smaller package (MEU6 /
QFN88, WEU6 / QFN68) would need a per-board pin list.

Modes are `IN`, `OUT`, `OPEN_DRAIN`, `ALT`, `ALT_OPEN_DRAIN` and `ANALOG`;
pulls are `PULL_UP`, `PULL_DOWN` and `None`; `drive()` takes `DRIVE_0`
through `DRIVE_3` and maps to the H417's per-pin `SPEED` register.
`machine.Signal` works, since `Pin` implements the pin protocol.

**A pull and an output level are the same bit of silicon.** For an input with
CNF = 10 the pull direction is taken from the output latch, so `pull=` is
accepted only in `IN` mode, and `value()` must decode the pin's direction
before choosing which register to read — reading `OUTDR` unconditionally makes
every pulled-up input report 1 regardless of what its pin is doing.

Interrupts use EXTI, which has 16 lines, one per pin *number*. Pin 0 of every
port shares line 0, so only one of PA0/PB0/…/PF0 can carry an interrupt at a
time; claiming a line another port holds raises `ValueError` rather than
silently retargeting it. `hard=True` runs the handler in the ISR.

Nothing stops you configuring PA9/PA10 (REPL), PA11/PA12 (USB) or PB8/PB9
(SWCLK/SWDIO). The debug probe holds that last pair while it is attached, so
a peripheral pointed at them configures perfectly and the pin never moves —
which looks exactly like a broken driver until you notice which pins they are.

**PB0 and PB1 are tied together on this dev board.** Found by driving each
candidate pin low in turn with every other pin pulled up: no other pair on the
headers is connected. Drive only one of them, or they fight.

## I2C

    from machine import I2C
    i2c = I2C(1, freq=400000)        # SCL=PB6, SDA=PB7
    i2c.scan()
    i2c.writeto(0x3C, b"\x00\xAF")
    i2c.readfrom(0x3C, 1)

`I2C(1)` through `I2C(4)` map to the hardware peripherals; `SoftI2C` bit-bangs
any two pins. Default pin pairs, with the alternatives the mux can reach:

| Bus | Default | Alternative | Domain |
|-----|---------|-------------|--------|
| 1 | PB6 / PB7 | PB8 / PB9 (also SWCLK/SWDIO) | **VDDIO 3.3 V** |
| 2 | PB10 / PB11 | PC0 / PC1 | VIO18 |
| 3 | PA8 / PC9 | PA14 / PA13 | mixed / VIO18 |
| 4 | PD12 / PD13 | PF12 / PF13 | VIO18 |

**PB6/PB7 is the only I2C pair in the 3.3 V domain.** Everything else is on
VIO18, which comes up well below 3.3 V, so an ordinary 3.3 V sensor on those
pins needs level shifting. PB6/PB7 are also 5 V tolerant (FT in the pin table).

I2C is open-drain and needs external pull-ups; the internal ones are far too
weak. Most breakout modules already carry 4.7 k.

The clock divider comes from the vendor `I2C_Init()`, which derives it from
`HCLK_Frequency` — the clock this peripheral really runs on.

An earlier version of this port replaced that with hand-written register setup
driven by `SystemCoreClock`, because the bus measured exactly 4.00x the
requested rate. **That was wrong**, and the mistake is worth remembering: the
measurement timed transfers with `mp_hal_ticks_us()`, and SysTick was itself
misconfigured from `SystemCoreClock` when it counts at HCLK, so the whole
timebase read 4x short. "The peripheral runs at 4x HCLK" and "the clock
measuring it runs 4x slow" fit that data equally well. Timing a device-side
`sleep_ms()` against a host stopwatch separated them — see "Timebase" below.
Do not measure a clock with a clock derived from the same suspect source.

Creating a `SoftI2C` on PB6/PB7 reconfigures those pins to plain GPIO, which
disconnects the hardware peripheral. Re-create the `I2C` object to take them
back.

`test_i2c.py` verifies all of this against an SSD1306 OLED, and skips when
nothing is attached. `framebuf` is enabled, so micropython-lib's display
drivers work as-is.

## ADC

    from machine import ADC, Pin
    a = ADC(Pin.cpu.PA7)
    a.read_u16()     # 0-65535, full scale
    a.read_uv()      # microvolts, referenced to the 3.3 V rail

ADC1, one conversion on demand, 12 bits. Channels are PA0-PA7 (IN0-IN7),
PB0-PB1 (IN8-IN9) and PC0-PC5 (IN10-IN15); any other pin raises `ValueError`.
The converter is calibrated once at first use, and the longest sample window is
always used, since the source impedance of whatever is attached is unknown and
a short window reads low on a high-impedance source.

`read_u16()` replicates the top bits rather than shifting in zeros, so full
scale really is 65535 rather than 65520.

Verified with an MCP4725 DAC on I2C1 whose output feeds PA7, so the two
peripherals check each other — see `test_adc.py`. Across the full range the
two agree to within **4 mV**:

| DAC code | expected | measured |
|---|---|---|
| 0 | 0 mV | 0 mV |
| 1024 | 825 mV | 825 mV |
| 2048 | 1650 mV | 1653 mV |
| 3072 | 2475 mV | 2479 mV |
| 4095 | 3300 mV | 3298 mV |

## Watchdog

    from machine import WDT
    w = WDT(timeout=5000)     # 1 ms to 26 s
    w.feed()

    import machine
    if machine.reset_cause() == machine.WDT_RESET:
        print("the watchdog fired last time")

`WDT(0)` is the independent watchdog (IWDG). It runs from the LSI, its own RC
oscillator, so it keeps counting even if the PLL drops out or the code that
was feeding it has stopped the bus clock. Once started it cannot be stopped —
not by `deinit()`, not by a soft reset, only by the reset it eventually
causes.

The LSI is an uncalibrated RC oscillator: the datasheet gives **25–60 kHz**,
and this port sizes the reload for a nominal 40 kHz. So a nominal timeout is
only good to about a factor of two across parts and temperature. Measured on
this board against the host clock, timing two different timeouts and
differencing them to cancel the boot overhead, the LSI is **41 kHz** — a 1 s
watchdog fires at 0.98 s and a 3 s one at 2.92 s. Leave margin.

**The window watchdog is deliberately not exposed.** WWDG counts HCLK/4096
through seven bits, so its whole range is 0.04 to 20.6 ms, and that ceiling is
hardware rather than a configuration choice. Measured here, a Python loop
calling `feed()` every 10 ms still misses the deadline, because a sleep plus
the USB poll in the VM hook can overshoot 20 ms. A watchdog that resets a
healthy board is worse than no watchdog.

`machine.reset_cause()` is real now rather than a stub, returning
`PWRON_RESET`, `HARD_RESET`, `WDT_RESET` or `SOFT_RESET`. The RCC flags
accumulate across resets, so they are read and cleared once at startup —
reading them later would report every reason since power-on.

## RTC

    from machine import RTC
    import time

    rtc = RTC()                                     # LSE, already running
    rtc.datetime((2026, 8, 9, 0, 14, 30, 0, 0))     # y, m, d, weekday, h, m, s, us
    print(rtc.datetime())
    print(time.localtime(), time.time())

The clock is started at boot, so `time.time()` and `time.localtime()` work
without creating an `RTC` object; creating one is how you set the date or pick a
different oscillator. Element 3 of the tuple is the weekday, which is derived
from the date rather than stored — the hardware has nowhere to keep it — and
element 7 is microseconds within the second, read from the prescaler's divider.

**No year 2038 problem.** The counter is 32 bits holding seconds since
**2000-01-01**, unsigned, so it runs out in **2136**. WCH's own RTC example
stores a Unix timestamp and converts it through a signed `time_t`, which is what
breaks in 2038 ([openwch/ch32h417#11]). The signedness is the whole bug, and
using MicroPython's own epoch means the number in the counter is exactly what
`time.time()` returns, with no offset to get wrong. Dates outside 2000–2135
raise `ValueError` rather than wrapping into a year the counter can hold.

[openwch/ch32h417#11]: https://github.com/openwch/ch32h417/issues/11

Three clock sources, chosen with `RTC(source=...)`:

| Source | Rate | Measured error | Notes |
|---|---|---|---|
| `RTC.LSE` | 32768 Hz | +0.04% | **Default.** Divides to exactly one second. The only source in the backup domain, so the only one that keeps time when VDD33 goes away but VBAT does not. This board has the crystal (Y1, 12 pF caps, PC14/PC15). |
| `RTC.LSI` | 40000 Hz nominal | **+3.2%** | Internal RC, needs no crystal. Specified only to 25–60 kHz, and measures 41.3 kHz here — so it gains about three quarters of an hour a day. Use it for elapsed time, not for a calendar. |
| `RTC.HSE` | 48828 Hz | +0.05% | The 25 MHz crystal over 512, which is 48828.125 Hz and so not a whole number of ticks per second; the nearest divisor gains ~2.6 ppm, a fifth of a second a day. Stops with VDD33. |

The measured column is against the host clock over 20 s, where the tolerance is
set by the serial round trip rather than by the oscillator — see `test_rtc.py`.
The LSI figure is real, though, and matches the 41.3 kHz measured independently
while bringing up the watchdog.

**Changing source resets the clock.** `RTCSEL` cannot be changed once written;
only a backup-domain reset reopens it, and that clears the counter. Asking for
the source already running keeps the time, which is what happens on every boot.

The clock survives both soft and hard resets — verified to the second — because
the backup domain is not reset by either. It does not survive removing power
unless VBAT is held up.

File timestamps come from here too: `get_fattime()` reports the RTC, so files
written after the date is set carry it.

## DAC

    from machine import DAC, Pin
    d = DAC(1)              # or DAC(Pin("PA4"))
    d.write(2048)           # 12 bits, 0-4095 -> 0 to 3.3 V
    d.write_uv(1650000)     # or say it in microvolts
    d.deinit()

Two independent 12-bit converters with their own output amplifiers, on **fixed
pins** — there is no mux to move them:

| | Pin | ADC channel on the same pin |
|---|---|---|
| `DAC(1)` | **PA4** | IN4 |
| `DAC(2)` | **PA5** | IN5 |

Both are in the 3.3 V supply domain. The amplifier is enabled, so the output
drives a 5 kΩ load; unlike the STM32 part, this one is near rail to rail with
the buffer on (datasheet: 0–8 mV at code 0, 3.29–3.3 V at code 4095).

`write_uv()` is not part of the `machine.DAC` API. It is here because this
port's ADC has `read_uv()`, and a converter whose full scale is only implied
by its bit count is awkward to use against one that reports volts.

A soft reset disables both channels. Otherwise the pin would hold its last
voltage indefinitely, with the `DAC` object that set it already collected.

Measured against the on-chip ADC with PA4 wired to PA6 (`test_dac.py`), the
two agree to **1.6 mV worst case** across the range:

| code | expected | measured |
|---|---|---|
| 0 | 0 mV | 0 mV |
| 256 | 206.3 mV | 204.7 mV |
| 1024 | 825.2 mV | 826.0 mV |
| 2048 | 1650.4 mV | 1650.4 mV |
| 3072 | 2475.6 mV | 2476.4 mV |
| 4095 | 3300.0 mV | 3299.2 mV |

Both converters share a reference, so this shows linearity and monotonicity
rather than absolute accuracy — a common reference error would cancel. The
MCP4725 comparison in `test_adc.py` is what pins the absolute scale down.

## SPI

    from machine import SPI, Pin
    cs = Pin("PA4", Pin.OUT)
    cs.value(1)
    spi = SPI(1, baudrate=1000000, polarity=0, phase=0)   # SCK=PA5, MISO=PA6, MOSI=PA7

    cs.value(0)
    spi.write(b"\xd0")          # BMP280 chip-id register, read bit set
    print(spi.read(1))
    cs.value(1)

`SPI(1)` through `SPI(4)` map to the hardware controllers; `SoftSPI` bit-bangs
any three pins. Chip select is **not** handled by either — drive it with a
`Pin`, as on every other MicroPython port. Hardware NSS would tie a bus to one
device, which stops being useful the moment there are two.

| Bus | Default SCK / MISO / MOSI | Domain |
|-----|---------------------------|--------|
| 1 | PA5 / PA6 / PA7 | **3.3 V** |
| 2 | PB13 / PB14 / PB15 | VIO18 |
| 3 | PB3 / PC11 / PC12 | VIO18 |
| 4 | PE2 / PE5 / PE6 | **3.3 V** |

`sck=`, `miso=` and `mosi=` select any other pin the mux can reach for that
bus; anything else raises `ValueError`. SPI1 also reaches PB3/PB4/PB5,
PF5/PF3/PD7 and PF7/PF9/PF8.

**SPI1 and SPI4 are the buses whose default pins are in a 3.3 V domain.** For
SPI1 that is measured, not read off the datasheet, whose supply domains are
only distinguishable by colour in the package figure: driving PA5, PA6 and PA7
high and reading each back through the ADC gives 3.299 V. The level of the
VIO18 pins is still open — see `docs/hw/ch32h417-notes.md` — but it is well
below 3.3 V either way, so treat those pins as needing level shifting.

Baudrate is `HCLK >> (n + 1)` for n in 0..7, so 50 MHz down to 390625 Hz in
powers of two, and an arbitrary request cannot be met exactly. The driver
rounds **down** — a device with a documented maximum SCK must not be
overclocked because the arithmetic landed one step high — and `print(spi)`
reports what it actually got:

| requested | actual |
|---|---|
| 1000000 | 781250 |
| 4000000 | 3125000 |
| 12500000 | 12500000 |
| 100000 | 390625 — **faster than asked** |

The last row is the one exception, and it is worth knowing about: below
390625 Hz there is no slower divider, so the request is clamped *up*. That
matches what the other ports do and is what the upstream test suite expects,
but it means `print(spi)` is the only reliable answer at low rates. Use
`SoftSPI` if you genuinely need a slow bus.

Only `bits=8` is supported. The peripheral can do 16-bit words, but with a byte
order the buffer protocol does not describe, so exposing it would be a trap.

**Every rate runs at the line rate, in both directions, because transfers go
through DMA.** Measured over 64 KB:

| SCK | `write` | `readinto` | `write_readinto` |
|---|---|---|---|
| 3.125 MHz | 390 kB/s (99%) | 390 kB/s (99%) | 390 kB/s (99%) |
| 12.5 MHz | 1562 kB/s (99%) | 1557 kB/s (99%) | 1561 kB/s (99%) |
| 25 MHz | 3123 kB/s (99%) | 3104 kB/s (99%) | 3122 kB/s (99%) |
| 50 MHz | 6242 kB/s (99%) | 6168 kB/s (98%) | 6240 kB/s (99%) |

A polling loop cannot do this, and no amount of tuning would have let it. At
`HCLK/2` a byte is 160 ns while one register access across the bus matrix costs
about 200 ns, so the CPU is beaten by the peripheral it is feeding. Polling
measured 1586 kB/s writing and 570 kB/s full duplex — flat ceilings that did not
move with the prescaler — and above 6.25 MHz full duplex lost bytes outright and
raised `OSError: SPI receive overrun`. Both limits are gone.

DMA reaches every memory region on this part, so no bounce buffer is involved
and a `bytes` literal in flash transmits as directly as a `bytearray` on the
heap. (An earlier revision of this file claimed DMA could not address the GC
heap in DTCM. That is true of the *USB* controller's bus master, which is why
USB has its own buffer in the shared region, and not of DMA1/DMA2 — the
reference manual gives them their own permission bits, enabled from reset, and
a memory-to-memory transfer inside the heap confirms it.)

The core **sleeps** for the duration of a transfer rather than spinning: the
waiting loop idles in `WFI` and the channel's transfer-complete interrupt wakes
it. At 390 kHz a full buffer is over a second that no longer runs the CPU flat
out.

Writes shorter than 16 bytes still go out through a short polled loop, because
arming two DMA channels costs about 3 µs and at that length it is not worth it.
Anything that receives always uses DMA, however short, so that a read does not
have a length below which it starts failing.

Creating a `SoftSPI` on SPI1's pins reconfigures them to plain GPIO and
disconnects the hardware peripheral, exactly as `SoftI2C` does. Re-create the
`SPI` object to take them back.

`test_spi.py` verifies the protocol side against a BMP280/BME280 and skips when
nothing is attached. `test_spi_dma.py` checks the throughput, the chunking of
buffers longer than a DMA counter, and the short-transfer path; it needs no
device, and picks up a MOSI-to-MISO loopback wire if one is fitted.

## PWM

    from machine import PWM, Pin
    p = PWM(Pin("PB0"), freq=1000, duty_u16=32768)   # 1 kHz, 50 %
    p.freq(50)
    p.duty_ns(1_500_000)                             # a servo at mid travel
    p.deinit()

The standard API: `freq()`, `duty_u16()`, `duty_ns()`, `init()`, `deinit()`
and `invert=`. **67 of the 95 pins** have a timer output; the rest reach a
timer only through ETR or BKIN, which are inputs, and raise `ValueError`.

Ten timers drive them — TIM1 and TIM8 (advanced), TIM2–TIM5, and TIM9–TIM12,
which on this part have four channels each rather than the two their STM32
namesakes have. TIM6 and TIM7 have no output pins at all.

Timers run from **HCLK, 100 MHz**, giving 1 Hz to 50 MHz. The prescaler is
kept as small as the period allows, because the period is also the duty
resolution: at 1 kHz the counter runs to 50000, so `duty_u16` is exact to
about a part in 50000.

Most pins reach two or three timers and the driver picks one — preferring a
timer already running at the frequency you asked for, then an idle timer, then
any free channel. Pass `timer=N` to choose, which is what you want when two
outputs need unrelated frequencies:

    a = PWM(Pin("PB0"), freq=1000)              # picks TIM3
    b = PWM(Pin("PC6"), freq=2000, timer=8)     # TIM3_CH1 would have moved a

**The frequency belongs to the timer, not the channel.** `freq()` on one
channel moves every other channel of that timer — the same caveat every port
carries. What this port adds is that the other channels keep their duty across
the change rather than being silently rescaled.

Pins named `TIMx_CHyN` in the datasheet are the complementary half of a
channel. They work, and `duty_u16` means the same fraction-of-time-high there
as anywhere else, which is what makes PA5, PB13, PE8, PE10 and PE12 usable at
all. Each `(timer, channel)` pair takes one PWM object, so a `CHy` and its
`CHyN` cannot be driven separately — they share a compare register.

A soft reset stops every output and releases its pin. So does `deinit()`,
which hands the pin back as a floating input rather than leaving it parked at
whatever level the last compare produced.

`test_pwm.py` checks all of this by measuring the waveform with
`time_pulse_us()` on the pin that is generating it — the pad's input buffer
stays live in alternate-function mode — rather than by reading back the
registers it just wrote.

## Backup memory

    import machine
    mem = machine.mem_backup()      # a writable memoryview, 128 bytes
    mem[0] = 0x42
    print(len(mem), mem.itemsize)   # 128 1

`machine.mem_backup()` returns 128 bytes that **survive a soft reset and
`machine.reset()`, and are lost when power goes away** — the same guarantee
esp32, rp2 and nrf give.

It is ordinary SRAM, in its own `NOLOAD` section placed between `.bss` and the
heap. Being in neither `.data` nor `.bss` is the whole mechanism: those are the
two things startup copies and clears, so a section in neither comes through a
reset untouched. It sits below `_heap_start`, so the GC never sees it.

There is nothing battery-backed to use instead. The datasheet mentions "the
backup register" alongside the RTC, but no such register block appears in the
reference manual's register tables, the SDK has no header for one, and probing
the address STM32F1 uses (`0x40006C00`) finds nothing writable — twenty slots
read back as zero after being written, with PWR and BKP clocked and `DBP` set.
The only thing in this chip's backup power domain that holds a value is the RTC
counter itself.

After a power cycle the SRAM would otherwise come back as whatever it settled
to, which is indistinguishable from real data. A magic word in front of the user
area turns that into a defined result: if it does not match, the region is
zeroed, so **a cold boot reads zeros** and a warm one reads what was there.

The size lives in `machine_mem_backup.c` and nowhere else: the linker script
places the section but does not reserve a length, so the two cannot drift apart
and let a write run past the reservation into the heap.

`test_mem_backup.py` stamps every byte and checks them back after both a soft
and a hard reset.

## Frozen modules

`boards/manifest.py` lists the Python modules compiled into the firmware, so
they import on a board whose filesystem has just been erased:

    dht         DHT11/DHT22 driver, the Python half of machine.dht_readinto()

`DHT11.temperature()` returns a whole number because the sensor sends one:
the frame carries a fraction byte after each of humidity and temperature, and
this part transmits `0f 00 19 00 28` — 15 %RH, 25 °C, both fractions zero,
checksum `0x28`. There is no half-degree to recover. `DHT22`/AM2302 uses
those bytes and reads to 0.1 °C.

Add more with `require("<name>")` for anything in `lib/micropython-lib`, or
`module("foo.py")` for a file of your own; a board can point `FROZEN_MANIFEST`
at its own manifest from `mpconfigboard.mk`.

Changing whether the port freezes anything at all changes `CFLAGS`, which the
build does not track — `py/frozenmod.c` compiles to an empty object without
`MICROPY_MODULE_FROZEN_MPY` and the link then fails on
`mp_find_frozen_module`. Run `make clean` after adding or removing
`FROZEN_MANIFEST`; editing the manifest itself needs no clean.

## Text

`str` is UTF-8 (`MICROPY_PY_BUILTINS_STR_UNICODE`), so `print("25°C")` puts
the two bytes `c2 b0` on the wire and `len()` counts characters. That is
above this port's `CORE_FEATURES` ROM level and is enabled deliberately: with
it off a degree sign left the board as a lone `0xb0`, which any terminal
expecting UTF-8 simply drops.

**Non-ASCII cannot be typed at the REPL.** The line editor ignores every byte
outside 32–126 (`shared/readline/readline.c`), because its cursor arithmetic
counts bytes and would otherwise walk into the middle of a character. This is
upstream behaviour, not specific to this port. At the REPL, type the escape
instead:

    >>> print("25\u00b0C")
    25°C

In a `.py` file the character itself works; only the line editor is affected.

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

The erase unit is 8 KB while FAT writes 512-byte sectors, so a sector write is
always read-modify-erase-program of a whole page. There is an 8 KB staging
buffer for that, and it coalesces a run of sectors within one `writeblocks()`
call into a single erase — but it is **not** a write-back cache. Every write
commits before the call returns, so once a block-device operation completes,
flash matches what the filesystem thinks it wrote. `os.sync()` is therefore a
no-op in practice. The block device is exposed as `ch32.Flash()`.

This used to defer the flush until something asked for a sync, and it
corrupted volumes: mounting discarded whatever was still dirty (and the
remount after every soft reset went through that path), USB MSC only flushed
on eject, and a debugger reset gives firmware no notice at all. Writes commit
eagerly now for that last reason above all — nothing the firmware can do
covers an OpenOCD reset.

The cost is write amplification. A 512-byte sector write is ~22 ms and bulk
writes plateau near 22 KB/s. (Those were recorded as 5.5 ms and 87 KB/s before
the SysTick fix described under "Timebase" — the board was timing itself with a
clock that ran 4x slow.) The volume is formatted with 512-byte clusters,
which stops FatFS batching, since it clamps multi-sector writes at the cluster
boundary; formatting with 8 KB clusters would recover most of the throughput
at a cost of 8 KB per file on a 505 KB volume. Rewrites of identical content
skip the erase entirely, which matters because FAT rewrites its allocation
table constantly.

**FAT on raw flash is still not power-loss safe.** An interruption between the
erase and the reprogram loses that 8 KB page. Closing that window needs a
log-structured format; FAT was chosen over littlefs because the volume is
exposed over USB MSC and hosts cannot read littlefs.

Note that MSC durability also depends on the *host*: Windows caches FAT
structures, so writes may not have reached the board at all until it flushes.
That part is outside the firmware's control.

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

## Idle power

Both cores gate their clock when idle rather than spinning.

The **V3F** parks in the WFI inside `PWR_EnterSTOPMode()` immediately after
waking the V5F, and stays there. It does not reach the loop after that call.

The **V5F** waits in WFI whenever the REPL has no input, which is most of a
board's life. This is what `MICROPY_INTERNAL_WFE` is for; before it was
defined, `mp_hal_stdin_rx_chr()` polled `mp_event_handle_nowait()` in a tight
loop and the core ran flat out at 400 MHz doing nothing. `time.sleep()` waits
the same way instead of spinning on the tick counter.

`__WFI()` clears the deep-sleep select bit before the instruction, so this
gates the core clock only — it does not ask the SoC to stop anything the other
core is still using.

The timeout passed to `MICROPY_INTERNAL_WFE` is ignored deliberately. SysTick
already interrupts every 1 ms, so no WFI lasts longer than that, which bounds
the usual wait-for-event race: an event that becomes ready between the check
and the WFI costs at most 1 ms of extra latency rather than a missed wakeup.

To check a core really is asleep, halt it over SWD and read `pc` — the two
targets are `wch_riscv.cpu.0` (V3F) and `wch_riscv.cpu.1` (V5F). A sleeping
core reports the instruction *after* its `wfi`, and reports the same address
every time; a busy one lands somewhere in `mp_execute_bytecode`.

    openocd -f wch-dual-core.cfg -c init -c "targets wch_riscv.cpu.1" \
            -c halt -c "reg pc" -c resume -c shutdown

## Timebase

SysTick counts at **HCLK**, which on this board is `SYSCLK >> 2` = 100 MHz —
*not* `SystemCoreClock`, which is the V5F core clock at 400 MHz. `mp_hal_init()`
configures the counter from `RCC_GetClocksFreq()`'s `HCLK_Frequency` for that
reason.

This was wrong for a long time, and it mattered more than it looks. Every tick
was worth four times what the rest of the port assumed, so:

- `time.sleep_ms(2000)` actually slept eight seconds.
- `time.ticks_ms()` / `ticks_us()` ran at a quarter speed.
- `time_pulse_us()` read 4x short, so every bit-banged protocol misjudged its
  timing. A DHT11 decoded as 40 zero bits, because a 70 us high measured as
  17 us and fell under the 48 us threshold separating a one from a zero.
- Anything the board measured about itself — bus speeds, benchmarks, flash
  throughput — was 4x optimistic.

The bug is invisible to any self-consistent test: `sleep_ms(100)` measured with
the same broken clock still reports 100 ms. It took an external reference to
see it. **To check the timebase, time a device-side sleep against the host:**

    t0 = time.monotonic()
    pyb.exec_("import time; time.sleep_ms(2000)")
    print((time.monotonic() - t0) * 1000)     # must be ~2000, was ~8000

A second, independent timebase bug lived here afterwards, and `sleep_ms()` is
blind to it too — check `sleep_us()` the same way. `ticks_us()` reconstructs a
tick that has completed but not yet been serviced, by reading SysTick's
overflow flag. It used to *clear* that flag and bump the millisecond counter
itself, under an interrupt lock, on the assumption that clearing the flag also
cancelled the pending interrupt. It does not — NVIC latches the request when
the flag asserts — so the handler counted the same tick again.

The error grew with how hard the clock was polled, since the race is whether
the reconstruction beats the handler to the flag. A Python loop calling
`ticks_us()` every ~20 us ran 3% fast, which reads as jitter; a C loop polling
every ~0.1 us ran up to 25% fast. So `time.sleep_us(5000)` returned after
4060 us, and the SPI driver's own throughput figures were nonsense.

The fix is to cancel the pending interrupt too, so exactly one of the
reconstruction and the handler counts any given tick. Making the
reconstruction purely read-only instead — the obvious alternative — is wrong:
it can then account for only *one* outstanding tick, so the clock saturates
about 1 ms into any critical section and DHT decoding stops dead. With the
pending-clear in place, `sleep_us()` measures exact and a 20 ms interrupts-off
window spinning on `ticks_us()` measures 20008 us:

    t0 = time.monotonic()
    pyb.exec_("import time\nfor _ in range(200): time.sleep_us(5000)")
    print(time.monotonic() - t0)              # must be ~1.0, was ~0.81

## Measured

    text 196856   data 4388   bss 30812     heap ~228 KB
    core clock 400 MHz

    benchmark  ~1050 ms
    upstream tests: 623 passed / 0 failed (22002 testcases)

The benchmark times itself on the target with `ticks_ms()`, so every figure
recorded before the SysTick fix was 4x too fast — the "~320 ms" this file used
to quote was really ~1280 ms. The old "13.3x faster than the V3F" claim came
from comparing a 4x-inflated V5F number against a V3F baseline whose clock was
correct (there `SystemCoreClock` and HCLK are both 100 MHz, so the bug did not
bite). **That ratio needs re-measuring before it is quoted again**; on these
numbers it looks closer to 4x.

The benchmark also moves by ±10% between runs depending on what the USB host is
doing to the MSC volume, so treat smaller differences as noise — measure a
suspected regression against a control build in the same session rather than
against a number recorded earlier.

See `docs/hw/benchmarks.md` for the layout comparison.

## Floating point

Built `-march=rv32imafc_zba_zbb_zbc_zbs_xw -mabi=ilp32f`. Both cores report
the same `misa=0x40901127`, including a hardware single-precision FPU, paired with
`MICROPY_FLOAT_IMPL_FLOAT`. This differs from the soft-float `ilp32` in the
stock PlatformIO board definition, and is safe because the vendor SDK is
compiled from source with the same flags.

## Modules beyond the default set

This port's ROM level is `CORE_FEATURES`, which leaves out a few things that
matter more on a board than they do in general:

| Module / feature | Why it is on |
|---|---|
| `uctypes` | Lays a struct over a buffer or a peripheral register block without writing C. Enabling it also brings in 24 upstream tests. |
| `framebuf` | Display drivers from micropython-lib work as-is. |
| `machine.mem_backup` | See "Backup memory". |
| `time.time()`, `localtime()`, `mktime()` | Answered by the RTC. |
| Unicode `str` | So `"°C"` prints as `°C`. |

Example, reading a DHT frame back as named fields:

    import uctypes
    LAYOUT = {"hum": 0 | uctypes.UINT16, "temp": 2 | uctypes.UINT16, "sum": 4 | uctypes.UINT8}
    buf = bytearray(5)
    f = uctypes.struct(uctypes.addressof(buf), LAYOUT, uctypes.BIG_ENDIAN)

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

Ethernet, RTC alarms, and the RV32 native emitter (which can be enabled
later targeting plain RV32IMC — the core is a superset, so no `xw` support is
needed in the emitter).
