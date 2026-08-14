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

/* uctypes: the way to lay a struct over a buffer or a peripheral register block
 * without writing C. Not in this ROM level by default, but hard to do without
 * on a board whose point is talking to hardware -- and the driver work here has
 * already wanted it twice. */
#define MICROPY_PY_UCTYPES               (1)

/* machine.mem_backup(). The region is plain SRAM in its own NOLOAD section, so
 * it survives a reset but not power-off -- see machine_mem_backup.c for why
 * there is nothing battery-backed to use instead. */
#define MICROPY_PY_MACHINE_MEM_BACKUP    (1)
#define MICROPY_PY_MACHINE_MEM_BACKUP_INCLUDEFILE "ports/ch32/machine_mem_backup.c"
#define MICROPY_PY_ARRAY_SLICE_ASSIGN   (1)
#define MICROPY_PY_BUILTINS_BYTES_HEX   (1)

// str is UTF-8 rather than a byte string. Defaults to BASIC_FEATURES, so it
// was off: print("25\u00b0C") put a lone 0xb0 on the wire and any terminal
// expecting UTF-8 dropped the degree sign. With this on, the same literal is
// two bytes on the wire and arrives intact, len() counts characters rather
// than bytes, and indexing a string with non-ASCII in it stops splitting
// characters in half.
#define MICROPY_PY_BUILTINS_STR_UNICODE (1)

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

/* Native and viper code generation, plus @micropython.asm_rv32.
 *
 * ZBA is on because both cores implement it -- the port already compiles with
 * -march=...._zba_zbb_zbc_zbs -- and the emitter uses sh1add/sh2add/sh3add for
 * the indexed loads that dominate viper array access. ZCMP is not: that is a
 * Zc code-size extension and it is not in this chip's march string.
 *
 * Emitted code lands on the GC heap, so it executes from DTCM or from the
 * shared region depending on which area the allocation came from; both are
 * executable. The V5F has a 32K I-cache, so MP_PLAT_COMMIT_EXEC issues a
 * fence.i -- see mpconfigport.c. */
#define MICROPY_EMIT_RV32               (1)
#define MICROPY_EMIT_RV32_ZBA           (1)
#define MICROPY_EMIT_INLINE_RV32        (1)
void *ch32_commit_exec(void *buf, size_t len, void *reloc);
#define MP_PLAT_COMMIT_EXEC(buf, len, reloc) ch32_commit_exec(buf, len, reloc)
/* Keep the GC tracking the emitted text. py/mpconfig.h turns this off when a
 * port defines MP_PLAT_COMMIT_EXEC, because that normally means the port also
 * allocates the text itself; here the default GC-heap allocator is still in
 * use, and an untracked block is collectable while a pointer into its middle
 * is the only thing keeping a native function alive. */
#define MICROPY_PERSISTENT_CODE_TRACK_FUN_DATA   (1)
#define MICROPY_PERSISTENT_CODE_TRACK_BSS_RODATA (0)

#define MICROPY_PY_SYS_PLATFORM         "ch32"
// Defaults to EXTRA_FEATURES only, but the upstream test suite expects it.
#define MICROPY_PY_SYS_MAXSIZE          (1)
#define MICROPY_PY_BUILTINS_HELP        (1)
#define MICROPY_PY_BUILTINS_HELP_MODULES (1)

#define MICROPY_PY_MACHINE              (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE  "ports/ch32/modmachine.c"
#define MICROPY_PY_MACHINE_BARE_METAL_FUNCS (1)
/* Real now that the port runs in Machine mode; in User mode the underlying
 * atomic section was a no-op and these would have lied. */
#define MICROPY_PY_MACHINE_DISABLE_IRQ_ENABLE_IRQ (1)
#define MICROPY_PY_MACHINE_RESET        (1)
#define MICROPY_PY_MACHINE_MEMX         (1)
/* time_pulse_us(), and the C bit-banger in drivers/dht/dht.c that builds on
 * it. Both need the microsecond timebase to survive a critical section. */
#define MICROPY_PY_MACHINE_PULSE        (1)

/* machine.bitstream(), which is what the frozen neopixel driver drives WS2812
 * strips with. The timed pin loop is ports/ch32/machine_bitstream.c. */
#define MICROPY_PY_MACHINE_BITSTREAM    (1)

/* I2C. The hardware peripheral lives in ports/ch32/machine_i2c.c; SoftI2C
 * comes from extmod and bit-bangs any two pins, which is the fallback for the
 * pin pairs the I2C mux cannot reach. */
#define MICROPY_PY_MACHINE_I2C          (1)
#define MICROPY_PY_MACHINE_SOFTI2C      (1)

/* SPI. Same split as I2C: the four hardware controllers live in
 * ports/ch32/machine_spi.c, and SoftSPI from extmod bit-bangs any three pins
 * for the combinations the AF mux cannot reach. Chip select is not part of
 * either -- callers drive CS with a Pin, as on every other port. */
#define MICROPY_PY_MACHINE_SPI          (1)
#define MICROPY_PY_MACHINE_SOFTSPI      (1)

/* PWM on the ten timers that have output pins (TIM1-TIM5, TIM8-TIM12; TIM6
 * and TIM7 have none). ports/ch32/machine_pwm.c owns the pin-to-channel table
 * and picks a timer for the pin unless one is named. */
#define MICROPY_PY_MACHINE_PWM          (1)
#define MICROPY_PY_MACHINE_PWM_INCLUDEFILE "ports/ch32/machine_pwm.c"

/* ADC1, single conversion on demand. read_uv() is exposed because the raw
 * 16-bit reading is meaningless without knowing the reference. */
#define MICROPY_PY_MACHINE_ADC          (1)
#define MICROPY_PY_MACHINE_ADC_INCLUDEFILE "ports/ch32/machine_adc.c"
#define MICROPY_PY_MACHINE_ADC_READ_UV  (1)

/* machine.UART on USART2-8, interrupt-driven in both directions through ring
 * buffers, with hardware RTS/CTS. USART1 is deliberately excluded: it carries
 * the REPL console and is owned by uart.c. Pin choice is how this part does
 * "remapping" -- it has an STM32F4-style per-pin AF mux, so any pin the
 * datasheet lists for a signal works. See machine_uart_pins.h. */
/* Without this the stream layer raises OSError(EAGAIN) when a read times out,
 * instead of returning None as UART.read() is documented to do. */
#define MICROPY_STREAMS_NON_BLOCK       (1)
#define MICROPY_PY_MACHINE_UART         (1)
#define MICROPY_PY_MACHINE_UART_INCLUDEFILE "ports/ch32/machine_uart.c"
#define MICROPY_PY_MACHINE_UART_SENDBREAK (1)
#define MICROPY_PY_MACHINE_UART_READCHAR_WRITECHAR (1)

/* random and os.urandom(), both backed by the hardware TRNG.
 *
 * The seed is the point. Without one, random.random() returns the identical
 * sequence on every board from every reset, which is the classic embedded
 * footgun; MICROPY_PY_RANDOM_SEED_INIT_FUNC takes 64 bits from the TRNG at
 * import instead. random.* remains a PRNG after that -- code wanting fresh
 * entropy per call should use os.urandom(), where every byte comes from the
 * peripheral.
 *
 * Raw RNG words are NOT uniform on this part and are whitened in rng.c. Read
 * the measurements there before relying on any of this for anything that
 * matters. */
#define MICROPY_PY_RANDOM               (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS   (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC (ch32_rng_u64())
#define MICROPY_PY_OS_URANDOM           (1)
#ifndef __ASSEMBLER__
#include <stdint.h>
uint64_t ch32_rng_u64(void);
#endif

/* framebuf is what every display driver in micropython-lib builds on -- an
 * SSD1306 is unusable without it -- and it also lets the upstream suite run
 * its framebuf tests instead of skipping them. */
#define MICROPY_PY_FRAMEBUF             (1)

/* Accelerate RGB565 fill and blit in ports/ch32/framebuf_accel.c: word-wise
 * C fast paths, plus the GPHA for buffers outside DTCM where it is the faster
 * of the two. See docs/hw/benchmarks.md for what each is worth. */
#define MICROPY_PY_FRAMEBUF_ACCEL       (1)

// time.sleep/ticks_* ride on the SysTick HAL via extmod's default bodies,
// which call mp_hal_delay_ms/us and mp_hal_ticks_ms/us/cpu directly.
#define MICROPY_PY_TIME                 (1)
#define MICROPY_PY_TIME_TICKS           (1)
/* time.time(), time.localtime() and friends, which come from the RTC via
 * mp_hal_time_ns(). Off until there was a real clock to answer them: a
 * time.time() that returns uptime is worse than one that is absent, because it
 * looks like a wall clock. See machine_rtc.c, including why the counter runs
 * out in 2136 rather than 2038. */
#define MICROPY_PY_TIME_TIME_TIME_NS    (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_INCLUDEFILE     "ports/ch32/modtime.c"
/* Dates past 2099. Off by default on 32-bit machines, where timeutils converts
 * through a 32-bit intermediate referenced to 1970: adding the 946684800 second
 * offset to a 2000-based timestamp then wraps, and 2134 reads back as 1997.
 * The RTC counter itself runs to 2136, so without this the last two years of
 * its range would silently report dates in the 1990s. */
#define MICROPY_TIME_SUPPORT_Y2100_AND_BEYOND (1)

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

/* I2S on the SAI peripheral. Most of the class is shared code in
 * extmod/machine_i2s.c; ports/ch32/machine_i2s.c supplies the hardware half. */
#define MICROPY_PY_MACHINE_I2S              (1)
#define MICROPY_PY_MACHINE_I2S_INCLUDEFILE  "ports/ch32/machine_i2s.c"
#define MICROPY_PY_MACHINE_I2S_RING_BUF     (1)
#define MICROPY_PY_MACHINE_I2S_CONSTANT_RX  (RX)
#define MICROPY_PY_MACHINE_I2S_CONSTANT_TX  (TX)

/* json: requests.json() needs it, and a board that talks to HTTP APIs without
 * being able to parse their replies is only half useful. */
#define MICROPY_PY_JSON                 (1)

/* binascii: hexlify/unhexlify and base64. Not in this ROM level by default,
 * but HTTP and TLS work expects it -- base64 for Basic auth, hex for keys and
 * digests -- and the AES known-answer tests are written in hex. */
#define MICROPY_PY_BINASCII             (1)
#define MICROPY_PY_BINASCII_CRC32       (1)

/* TLS on mbedtls, with AES on the ECDC accelerator and the hardware TRNG
 * seeding the entropy pool -- see ports/ch32/mbedtls/. */
#define MICROPY_PY_SSL                  (1)
#define MICROPY_SSL_MBEDTLS             (1)
/* mbedtls allocates through m_tracked_calloc/free so its buffers are visible
 * to the GC and released on soft reset; without this the link fails outright. */
#define MICROPY_TRACKED_ALLOC           (MICROPY_SSL_MBEDTLS)

/* hashlib: md5, sha1, sha256. The module itself needs enabling because it
 * defaults to EXTRA_FEATURES and this port is CORE_FEATURES -- without this
 * line the two algorithm selections below choose the contents of a module that
 * is never built, which is what they did until now.
 *
 * Nearly free: extmod/modhashlib.c calls mbedtls's implementations when
 * MICROPY_SSL_MBEDTLS is set, and TLS has already linked all three. There is
 * no sha384 or sha512 to enable -- modhashlib.c implements md5, sha1 and
 * sha256 and nothing else, so those would be new code rather than a flag,
 * even though mbedtls's SHA-512 is in the image for the TLS ciphersuites. */
#define MICROPY_PY_HASHLIB              (1)
#define MICROPY_PY_HASHLIB_SHA1         (1)
#define MICROPY_PY_HASHLIB_SHA256       (1)

/* cryptolib comes in with MICROPY_PY_SSL and its AES runs on the ECDC block,
 * because modcryptolib.c calls mbedtls_aes_crypt_ecb/_cbc and MBEDTLS_AES_ALT
 * points those at ports/ch32/mbedtls/aes_alt.c. CTR is the one mode that is
 * off by default. */
#define MICROPY_PY_CRYPTOLIB_CTR        (1)

// Networking: lwip on the on-chip Ethernet MAC and 100M PHY.
#define MICROPY_PY_NETWORK              (1)
#define MICROPY_PY_NETWORK_LAN          (1)
#define MICROPY_PY_SOCKET               (1)
/* extmod.mk already puts -DMICROPY_PY_LWIP=1 on the command line in response
 * to MICROPY_PY_LWIP=1 in the Makefile; this is only the fallback for a
 * translation unit that does not get CFLAGS_EXTMOD. */
#ifndef MICROPY_PY_LWIP
#define MICROPY_PY_LWIP                 (1)
#endif
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "mpy-ch32"
#define MICROPY_PORT_NETWORK_INTERFACES \
    { MP_ROM_QSTR(MP_QSTR_LAN), MP_ROM_PTR(&network_lan_type) },
extern const struct _mp_obj_type_t network_lan_type;

/* MICROPY_PY_LWIP_ENTER/EXIT are deliberately left at their empty defaults.
 *
 * Wrapping modlwip.c's socket calls in an interrupt mask looks like the right
 * thing and is a trap: those regions can raise -- mp_raise_OSError on a
 * timeout, for one -- and an exception unwinding past the EXIT leaves the
 * Ethernet interrupt masked forever, which kills the interface for good. No
 * upstream port defines these; stm32 and mimxrt both call netif->input
 * straight from the ETH ISR and rely on lwip's own SYS_ARCH_PROTECT. This
 * port follows them. What it does protect is sys_check_timeouts(), in
 * mpnetworkport.c, where the region is short and contains no raise. */

/* Background work has to be serviced from two places, and both are needed.
 *
 * MICROPY_INTERNAL_EVENT_HOOK runs from mp_event_handle_nowait(), which is what
 * the REPL spins on while waiting for input -- without it, a board sitting at
 * the prompt never processes USB or lwip timers at all.
 *
 * The VM hooks cover the other case: a long-running Python loop never reaches
 * the event hook, and the host would eventually drop the connection. */
void mp_hal_ch32_poll(void);
#define MICROPY_INTERNAL_EVENT_HOOK mp_hal_ch32_poll()

/* Idle by gating the core clock rather than spinning. Without this the core
 * runs flat out whenever the REPL is waiting for a character, which is most of
 * a board's life.
 *
 * The timeout is ignored on purpose: SysTick already interrupts every 1 ms, so
 * a WFI cannot last longer than that. It bounds the classic wait-for-event
 * race -- an event becoming ready between the check and the WFI costs at most
 * 1 ms of extra latency instead of sleeping forever. */
void mp_hal_ch32_wfe(void);
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS) mp_hal_ch32_wfe()
/* The ISR does the time-critical USB work; tud_task() only drains deferred
 * events, so polling it every 16 bytecodes cost ~18%% on the benchmark for no
 * benefit. 256 keeps the host happy and the overhead unmeasurable. */
#define MICROPY_VM_HOOK_COUNT (256)
#define MICROPY_VM_HOOK_INIT static uint vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL if (--vm_hook_divisor == 0) {         vm_hook_divisor = MICROPY_VM_HOOK_COUNT;                   mp_hal_ch32_poll();                                    }
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
