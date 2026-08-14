/* machine.RTC for the CH32H417.
 *
 * The peripheral is the STM32F1 one: a 32-bit second counter (CNTH/CNTL) fed
 * through a 20-bit prescaler, living in the backup domain so it keeps running
 * across a reset. There is no BCD calendar and no date arithmetic in hardware;
 * everything above "seconds" is done here, through shared/timeutils.
 *
 * --- the year 2038 -------------------------------------------------------
 *
 * WCH's own RTC example stores a Unix timestamp in that counter and converts it
 * with a signed time_t, which stops working on 19 January 2038
 * (openwch/ch32h417 issue 11). This driver does not have that problem, and not
 * because of a workaround: the counter is unsigned, and what it holds is
 * seconds since **2000-01-01 00:00:00 UTC**, which is already MicroPython's own
 * epoch on this port. 2^32 seconds is 136 years, so the counter runs out in
 * **2136**, and nothing in between needs an offset, an overflow interrupt or a
 * shadow copy.
 *
 * That is a better answer than re-basing to 2020 to buy 68 years, which is what
 * you get by keeping a *signed* interpretation. The signedness is the whole
 * bug; fixing that is worth more than moving the epoch. Choosing MicroPython's
 * epoch rather than an arbitrary one also means the number in the counter is
 * exactly what time.time() returns, so there is no second place for an
 * off-by-one-epoch mistake to hide.
 *
 * The Python side is unaffected either way: this port builds with arbitrary
 * precision integers, so time.time() keeps counting past any 32-bit boundary.
 *
 * --- clock sources -------------------------------------------------------
 *
 * RTCSEL picks one of three, and they are not interchangeable:
 *
 *   LSE        32768 Hz from the crystal at PC14/PC15. Divides to exactly one
 *              second, and is the only source in the backup domain, so it is
 *              the only one that keeps time when VDD33 goes away and VBAT does
 *              not. The default, because this board has the crystal fitted
 *              (Y1, with 12 pF loading capacitors).
 *   LSI        The internal RC. Needs no crystal and cannot be stopped, but it
 *              is only specified to 25-60 kHz -- it measured 41.3 kHz on this
 *              board -- so a nominal 40000 divisor can be several percent out.
 *              That is minutes a day. Fine for elapsed time, not for a clock.
 *   HSE/512    The 25 MHz crystal divided by 512, so 48828.125 Hz, which is not
 *              a whole number of ticks per second. The nearest divisor gains
 *              about 2.6 ppm, a fifth of a second a day. Accurate, but it stops
 *              with VDD33 and the datasheet does not guarantee the RTC state
 *              across that, so it is for boards with no 32 kHz crystal that
 *              still want a good clock while powered.
 *
 * RTCSEL cannot be changed once set without resetting the whole backup domain,
 * which clears the counter. Asking for a different source therefore restarts
 * the clock from 2000-01-01; asking for the one already running keeps the time.
 */
#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/mpirq.h"
#include "shared/timeutils/timeutils.h"

#include "irq.h"
#include "machine_rtc.h"

enum {
    MACHINE_RTC_SRC_LSE = 1,
    MACHINE_RTC_SRC_LSI = 2,
    MACHINE_RTC_SRC_HSE = 3,
};

/* Ticks per second of each source. The prescaler register takes this minus one:
 * the reference manual gives the division factor as PRL[19:0] + 1.
 *
 * HSE/512 is 48828.125 Hz and cannot be expressed here. 48828 is the closer of
 * the two whole numbers -- it makes the second 2.6 ppm short, where 48829 would
 * make it 18 ppm long. */
#define MACHINE_RTC_LSE_HZ (32768)
#define MACHINE_RTC_LSI_HZ (40000)
#define MACHINE_RTC_HSE_HZ (HSE_VALUE / 512)

/* How long to give an oscillator to start. The LSE is a watch crystal and takes
 * a few hundred milliseconds; this is generous enough to cover a cold one and
 * short enough that a board with no crystal fitted is not left hanging. */
#define MACHINE_RTC_STARTUP_MS (1500)

/* RTOFF and RSF both clear within a few RTC clock cycles, so this is three
 * orders of magnitude of slack. It exists because the register waits must not
 * be able to hang: see machine_rtc_wait_flag(). */
#define MACHINE_RTC_REG_MS     (100)

typedef struct _machine_rtc_obj_t {
    mp_obj_base_t base;
} machine_rtc_obj_t;

static const machine_rtc_obj_t machine_rtc_singleton = { { &machine_rtc_type } };

/* Which source is running, or 0 if the clock has not been started. */
static uint8_t machine_rtc_source;
static uint32_t machine_rtc_hz;

/* The alarm is EXTI line 17. Routing it there is not optional even while
 * awake, because that is also what wakes the chip out of Stop -- see the
 * comment on rtc_alarm_arm_at(). */
#define EXTI_LINE_RTC_ALARM (1u << 17)

/* Alarm state. There is one comparator, so there is one alarm.
 *
 * machine_rtc_alarm_period is the repeat interval in seconds, or 0 for a
 * one-shot; the hardware cannot repeat, so the handler re-arms. Both are
 * touched by the interrupt, hence volatile. */
static volatile uint32_t machine_rtc_alarm_target;
static volatile uint32_t machine_rtc_alarm_period;
static volatile bool machine_rtc_alarm_armed;

/* Read-modify-write BDCTLR and confirm it took. Returns false if it never does.
 *
 * Necessary because the backup domain runs from a much slower clock than the
 * 400 MHz core, and a write that arrives while the previous one is still
 * crossing is dropped without any indication. WCH's own RTC example gestures at
 * this with a bare NOP delay inserted only for the non-V3F cores. A fixed delay
 * is a guess; reading back until the value is there is not.
 *
 * Getting this wrong is not a transient glitch. The pair of writes that pulses
 * BDRST silently lost its second half, which left the backup domain held in
 * reset -- and with it held, every later write is dropped too, so the clock
 * could not be started again by anything short of a power cycle. */
static bool machine_rtc_bdctlr_write(uint32_t clear, uint32_t set) {
    mp_uint_t deadline = mp_hal_ticks_ms() + MACHINE_RTC_REG_MS;
    for (;;) {
        RCC->BDCTLR = (RCC->BDCTLR & ~clear) | set;
        uint32_t now = RCC->BDCTLR;
        if ((now & clear & ~set) == 0 && (now & set) == set) {
            return true;
        }
        if (mp_hal_ticks_ms() > deadline) {
            return false;
        }
    }
}

/* The SDK's RTC_WaitForLastTask() and RTC_WaitForSynchro() spin with no bound,
 * and both wait on flags driven by the RTC clock domain. If no oscillator is
 * actually driving that domain they never return -- at boot that is a board
 * that never reaches the REPL and says nothing about why, which is exactly how
 * the first version of this file failed. Every wait here is bounded. */
static bool machine_rtc_wait_flag(uint16_t flag) {
    mp_uint_t deadline = mp_hal_ticks_ms() + MACHINE_RTC_REG_MS;
    while (!(RTC->CTLRL & flag)) {
        if (mp_hal_ticks_ms() > deadline) {
            return false;
        }
    }
    return true;
}

/* RTOFF: the last write has reached the RTC clock domain and another may start.
 * Every write to CNT or PSCR has to be followed by this. */
static bool machine_rtc_wait_write(void) {
    return machine_rtc_wait_flag(RTC_FLAG_RTOFF);
}

/* RSF: the APB-side copies of CNT and DIV have been refreshed from the RTC
 * clock domain. Required after any reset of the APB interface, because until it
 * sets, reads return whatever was latched before. */
static bool machine_rtc_wait_sync(void) {
    RTC->CTLRL &= (uint16_t) ~RTC_FLAG_RSF;
    return machine_rtc_wait_flag(RTC_FLAG_RSF);
}

static uint32_t machine_rtc_source_hz(uint8_t source) {
    switch (source) {
        case MACHINE_RTC_SRC_LSE:
            return MACHINE_RTC_LSE_HZ;
        case MACHINE_RTC_SRC_LSI:
            return MACHINE_RTC_LSI_HZ;
        default:
            return MACHINE_RTC_HSE_HZ;
    }
}

/* What RTCSEL currently says, translated back into one of the ids above. Reads
 * the register rather than a variable because the backup domain survives a
 * reset: after a warm boot the clock is already running and the firmware that
 * started it is gone. */
static uint8_t machine_rtc_running_source(void) {
    uint32_t bd = RCC->BDCTLR;
    if (!(bd & RCC_RTCEN)) {
        return 0;
    }
    switch (bd & RCC_RTCSEL) {
        case RCC_RTCCLKSource_LSE:
            return MACHINE_RTC_SRC_LSE;
        case RCC_RTCCLKSource_LSI:
            return MACHINE_RTC_SRC_LSI;
        case RCC_RTCCLKSource_HSE_Div512:
            return MACHINE_RTC_SRC_HSE;
        default:
            return 0;
    }
}

/* Returns false if the oscillator never came ready. */
static bool machine_rtc_start_oscillator(uint8_t source) {
    if (source == MACHINE_RTC_SRC_HSE) {
        /* Already running: it is what the PLL is locked to. */
        return RCC_GetFlagStatus(RCC_FLAG_HSERDY) != RESET;
    }
    if (source == MACHINE_RTC_SRC_LSI) {
        /* LSI lives in RCC_CTLR, outside the backup domain, so it needs none of
         * the care above. */
        RCC_LSICmd(ENABLE);
    } else if (!machine_rtc_bdctlr_write(RCC_LSEBYP, RCC_LSEON)) {
        return false;
    }
    uint8_t flag = (source == MACHINE_RTC_SRC_LSI) ? RCC_FLAG_LSIRDY : RCC_FLAG_LSERDY;
    mp_uint_t deadline = mp_hal_ticks_ms() + MACHINE_RTC_STARTUP_MS;
    while (RCC_GetFlagStatus(flag) == RESET) {
        if (mp_hal_ticks_ms() > deadline) {
            return false;
        }
    }
    return true;
}

static bool machine_rtc_configure_inner(uint8_t source);

/* Configure the clock for `source`, keeping the running time if that source is
 * already selected. Returns false if it could not be started.
 *
 * A failed attempt must not leave the board worse off than it found it. The
 * wrapper exists so that every failure path ends with the domain out of reset
 * and the driver reporting no clock, rather than half-configured: an abandoned
 * attempt used to leave BDRST asserted, which made every subsequent attempt
 * fail too, so one bad call bricked the RTC until the next power cycle. */
static bool machine_rtc_configure(uint8_t source) {
    if (machine_rtc_configure_inner(source)) {
        return true;
    }
    machine_rtc_source = 0;
    machine_rtc_hz = 0;
    machine_rtc_bdctlr_write(RCC_BDRST, 0);
    return false;
}

static bool machine_rtc_configure_inner(uint8_t source) {
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_PWR | RCC_HB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);

    if (machine_rtc_running_source() == source) {
        /* Already ticking from the right oscillator, which is the normal case
         * after a warm reset. Leave the counter and prescaler alone: rewriting
         * them would throw away a clock that has been keeping time since before
         * this firmware started. */
        if (!machine_rtc_wait_sync()) {
            return false;
        }
        machine_rtc_source = source;
        machine_rtc_hz = machine_rtc_source_hz(source);
        return true;
    }

    /* Selecting a source needs a full backup-domain reset pulse first, and this
     * is the part that is easy to get wrong on this chip.
     *
     * RTCSEL cannot be *changed* once written; only a backup-domain reset
     * reopens it. Two things then conspire. This part comes out of power-on
     * with BDRST already asserted, unlike STM32, so every write to BDCTLR is
     * silently dropped until it is cleared -- and RCC_LSEConfig() writes a
     * whole byte to BDCTLR, RTCSEL bits included, so merely starting the LSE
     * counts as writing RTCSEL=00 and latches it there. After that the clock
     * can never be selected, every write reads back as zero, and nothing
     * reports an error. Pulsing BDRST here clears both the latch and the
     * power-on assertion, and costs nothing: the only time on the clock at this
     * point is time this call was going to replace anyway. */
    if (!machine_rtc_bdctlr_write(0, RCC_BDRST)
        || !machine_rtc_bdctlr_write(RCC_BDRST, 0)) {
        return false;
    }
    PWR_BackupAccessCmd(ENABLE);

    /* Oscillator first, selection second: starting the LSE writes BDCTLR, and
     * the SDK's RCC_LSEConfig() rewrites the whole low byte including RTCSEL,
     * so the other order would clear the selection just made. */
    if (!machine_rtc_start_oscillator(source)) {
        return false;
    }

    uint32_t sel = source == MACHINE_RTC_SRC_LSE ? RCC_RTCCLKSource_LSE
        : source == MACHINE_RTC_SRC_LSI ? RCC_RTCCLKSource_LSI
                                        : RCC_RTCCLKSource_HSE_Div512;
    if (!machine_rtc_bdctlr_write(RCC_RTCSEL, sel)
        || !machine_rtc_bdctlr_write(0, RCC_RTCEN)) {
        return false;
    }

    if (!machine_rtc_wait_write() || !machine_rtc_wait_sync() || !machine_rtc_wait_write()) {
        return false;
    }
    RTC_SetPrescaler(machine_rtc_source_hz(source) - 1);
    if (!machine_rtc_wait_write()) {
        return false;
    }

    machine_rtc_source = source;
    machine_rtc_hz = machine_rtc_source_hz(source);
    return true;
}

bool machine_rtc_get(uint32_t *seconds, uint32_t *microseconds) {
    if (machine_rtc_source == 0) {
        return false;
    }
    /* CNT and DIV are separate registers ticking off the same oscillator, so
     * they have to be sampled either side of the counter to know they belong to
     * the same second. Without this, a read landing on the boundary pairs a new
     * second with the old fraction and time appears to jump backwards by almost
     * a second. */
    uint32_t cnt, div, again;
    do {
        cnt = RTC_GetCounter();
        div = RTC_GetDivider();
        again = RTC_GetCounter();
    } while (cnt != again);

    *seconds = cnt;
    /* DIV counts down from PRL to 0 across the second, so the elapsed fraction
     * is (PRL - DIV) / (PRL + 1). In 64 bits because the numerator overflows a
     * 32-bit multiply for every source here. */
    uint32_t prl = machine_rtc_hz - 1;
    uint32_t elapsed = (div > prl) ? 0 : (prl - div);
    *microseconds = (uint32_t)(((uint64_t)elapsed * 1000000ull) / machine_rtc_hz);
    return true;
}

/* Point the comparator at an absolute counter value and enable the interrupt.
 *
 * The EXTI routing here is what machine_sleep.c used to do for itself, and it
 * belongs with the arming rather than beside one caller: in Stop the core and
 * its interrupt controller are clock-gated, so a peripheral interrupt cannot
 * reach them, and EXTI -- which is not gated -- is what restarts the clocks.
 * RM 2.3.4 says so in as many words: "the external break line 17 needs to be
 * configured". An alarm armed from Python must wake deepsleep() the same way
 * one armed by lightsleep() does, and having one place that arms is the only
 * way to be sure of that. */
static bool rtc_alarm_arm_at(uint32_t target) {
    if (machine_rtc_source == 0) {
        return false;
    }
    if (!machine_rtc_wait_write()) {
        return false;
    }
    RTC_ClearFlag(RTC_FLAG_ALR);
    RTC_SetAlarm(target);           /* enters and exits config mode itself */
    if (!machine_rtc_wait_write()) {
        return false;
    }
    RTC_ITConfig(RTC_IT_ALR, ENABLE);
    if (!machine_rtc_wait_write()) {
        return false;
    }

    EXTI->RTENR |= EXTI_LINE_RTC_ALARM;
    EXTI->INTFR = EXTI_LINE_RTC_ALARM;
    EXTI->INTENR |= EXTI_LINE_RTC_ALARM;
    NVIC_EnableIRQ(RTCAlarm_IRQn);

    machine_rtc_alarm_target = target;
    machine_rtc_alarm_armed = true;
    return true;
}

bool machine_rtc_alarm_in(uint32_t seconds) {
    if (machine_rtc_source == 0 || seconds == 0) {
        return false;
    }
    machine_rtc_alarm_period = 0;
    /* The counter is unsigned and free-running, so an alarm that wraps past
     * 2^32 still compares equal at the right moment -- no clamping needed. */
    return rtc_alarm_arm_at(RTC_GetCounter() + seconds);
}

void machine_rtc_alarm_clear(void) {
    machine_rtc_alarm_armed = false;
    machine_rtc_alarm_period = 0;
    if (machine_rtc_source == 0) {
        return;
    }
    if (!machine_rtc_wait_write()) {
        return;
    }
    RTC_ITConfig(RTC_IT_ALR, DISABLE);
    machine_rtc_wait_write();
    RTC_ClearFlag(RTC_FLAG_ALR);
    EXTI->INTENR &= ~EXTI_LINE_RTC_ALARM;
    EXTI->INTFR = EXTI_LINE_RTC_ALARM;
}

/* Seconds until the alarm, 0 if it has passed or none is armed. Unsigned
 * subtraction, so a target that wrapped past 2^32 still gives the right
 * distance. */
static uint32_t rtc_alarm_left_seconds(void) {
    if (!machine_rtc_alarm_armed || machine_rtc_source == 0) {
        return 0;
    }
    uint32_t now = RTC_GetCounter();
    uint32_t target = machine_rtc_alarm_target;
    return (target - now) > 0x80000000u ? 0 : (target - now);
}

/* The alarm interrupt.
 *
 * This also lands on the way out of Stop, where machine_sleep.c relies on it
 * existing at all: the core takes the vector as the clocks restart, and an
 * unhandled one would trap instead of returning to the instruction after the
 * WFI. It used to live there for exactly that reason and does no harm when no
 * Python handler is registered. */
void CH32_IRQ_HANDLER(RTCAlarm_IRQHandler);
void RTCAlarm_IRQHandler(void) {
    RTC_ClearITPendingBit(RTC_IT_ALR);
    EXTI->INTFR = EXTI_LINE_RTC_ALARM;

    if (machine_rtc_alarm_period != 0) {
        /* Step the comparator on from the target rather than from the counter,
         * so a repeating alarm does not drift by however long it took to get
         * here. RTC_SetAlarm() has to wait on RTOFF, which is slow in an
         * interrupt but is the only way this peripheral takes a write. */
        uint32_t next = machine_rtc_alarm_target + machine_rtc_alarm_period;
        if (machine_rtc_wait_write()) {
            RTC_SetAlarm(next);
            machine_rtc_alarm_target = next;
        }
    } else {
        machine_rtc_alarm_armed = false;
        RTC_ITConfig(RTC_IT_ALR, DISABLE);
    }

    mp_irq_obj_t *irq = MP_STATE_PORT(machine_rtc_irq_object);
    if (irq != NULL && irq->handler != mp_const_none) {
        mp_irq_handler(irq);
    }
}

/* Called from the soft-reset path. The handler lives on the heap that is about
 * to be reclaimed, so an alarm left armed would dispatch into freed memory the
 * moment it fired. */
void machine_rtc_irq_deinit(void) {
    machine_rtc_alarm_clear();
    MP_STATE_PORT(machine_rtc_irq_object) = NULL;
}

void machine_rtc_init_boot(void) {
    /* LSE by default because this board has the crystal. If it is missing, or
     * has not started, fall back to the internal RC rather than leaving the
     * board with no wall clock at all -- a clock that is a few percent out is
     * more use than none, and print(rtc) says which one is running. */
    if (!machine_rtc_configure(MACHINE_RTC_SRC_LSE)) {
        machine_rtc_configure(MACHINE_RTC_SRC_LSI);
    }
}

/* --- Python bindings --- */

static void machine_rtc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    (void)self_in;
    static const char *const names[] = { "none", "LSE", "LSI", "HSE/512" };
    mp_printf(print, "RTC(source=%s, %u Hz)",
        names[machine_rtc_source], (unsigned)machine_rtc_hz);
}

static void machine_rtc_set_datetime(mp_obj_t datetime_in) {
    mp_obj_t *item;
    size_t len;
    mp_obj_get_array(datetime_in, &len, &item);
    if (len < 3 || len > 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("datetime needs 3 to 8 elements"));
    }

    mp_int_t year = mp_obj_get_int(item[0]);
    if (year < 2000 || year > 2135) {
        /* The counter is seconds since 2000 in 32 unsigned bits. Refusing the
         * years it cannot hold is better than silently wrapping into one it
         * can. */
        mp_raise_ValueError(MP_ERROR_TEXT("year must be 2000 to 2135"));
    }
    /* Element 3 is the weekday, which is derived rather than stored -- there is
     * nowhere in the hardware to put it, and it is a function of the date. */
    mp_uint_t seconds = timeutils_seconds_since_epoch(
        year,
        mp_obj_get_int(item[1]),
        mp_obj_get_int(item[2]),
        len > 4 ? mp_obj_get_int(item[4]) : 0,
        len > 5 ? mp_obj_get_int(item[5]) : 0,
        len > 6 ? mp_obj_get_int(item[6]) : 0);

    if (!machine_rtc_wait_write()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("RTC is not accepting writes"));
    }
    RTC_SetCounter(seconds);
    if (!machine_rtc_wait_write()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("RTC write did not complete"));
    }
}

static mp_obj_t machine_rtc_datetime_helper(size_t n_args, const mp_obj_t *args) {
    if (machine_rtc_source == 0) {
        mp_raise_OSError(MP_EIO);
    }
    if (n_args == 1) {
        uint32_t seconds, microseconds;
        machine_rtc_get(&seconds, &microseconds);
        timeutils_struct_time_t tm;
        timeutils_seconds_since_epoch_to_struct_time(seconds, &tm);
        mp_obj_t tuple[8] = {
            mp_obj_new_int(tm.tm_year),
            mp_obj_new_int(tm.tm_mon),
            mp_obj_new_int(tm.tm_mday),
            mp_obj_new_int(tm.tm_wday),
            mp_obj_new_int(tm.tm_hour),
            mp_obj_new_int(tm.tm_min),
            mp_obj_new_int(tm.tm_sec),
            mp_obj_new_int(microseconds),
        };
        return mp_obj_new_tuple(8, tuple);
    }
    machine_rtc_set_datetime(args[1]);
    return mp_const_none;
}

static mp_obj_t machine_rtc_datetime(size_t n_args, const mp_obj_t *args) {
    return machine_rtc_datetime_helper(n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_rtc_datetime_obj, 1, 2, machine_rtc_datetime);

// RTC.init(datetime) -- an alias for the setting form of datetime().
static mp_obj_t machine_rtc_init(mp_obj_t self_in, mp_obj_t datetime_in) {
    (void)self_in;
    if (machine_rtc_source == 0) {
        mp_raise_OSError(MP_EIO);
    }
    machine_rtc_set_datetime(datetime_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(machine_rtc_init_obj, machine_rtc_init);

// RTC.now() -- CPython's field order, with microseconds where CPython has them.
static mp_obj_t machine_rtc_now(mp_obj_t self_in) {
    (void)self_in;
    if (machine_rtc_source == 0) {
        mp_raise_OSError(MP_EIO);
    }
    uint32_t seconds, microseconds;
    machine_rtc_get(&seconds, &microseconds);
    timeutils_struct_time_t tm;
    timeutils_seconds_since_epoch_to_struct_time(seconds, &tm);
    mp_obj_t tuple[8] = {
        mp_obj_new_int(tm.tm_year),
        mp_obj_new_int(tm.tm_mon),
        mp_obj_new_int(tm.tm_mday),
        mp_obj_new_int(tm.tm_hour),
        mp_obj_new_int(tm.tm_min),
        mp_obj_new_int(tm.tm_sec),
        mp_obj_new_int(microseconds),
        mp_const_none,
    };
    return mp_obj_new_tuple(8, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_rtc_now_obj, machine_rtc_now);

// RTC.source() -- which oscillator is driving the clock.
static mp_obj_t machine_rtc_source_fn(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_int(machine_rtc_source);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_rtc_source_obj, machine_rtc_source_fn);

/* --- alarm ---
 *
 * One comparator, so one alarm, and it compares whole seconds against the same
 * counter time.time() reads. Everything below is that single fact worked
 * through: ALARM0 is the only id, an interval in milliseconds rounds *up* to
 * the next second so an alarm never fires early, and repeat is done by the
 * handler re-arming because the hardware cannot.
 *
 * Note that lightsleep(ms) and deepsleep(ms) arm the same comparator once the
 * requested time is past the ~26 s that LPTIM covers, so a sleep with a long
 * timeout replaces a pending alarm. There is no second one to fall back on. */

static void rtc_alarm_check_id(mp_int_t id) {
    if (id != 0) {
        /* ENODEV rather than ValueError, which is what the other ports raise
         * for an alarm id the hardware does not have. */
        mp_raise_OSError(MP_ENODEV);
    }
}

// RTC.alarm(id, time, *, repeat=False)
static mp_obj_t machine_rtc_alarm(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_alarm_id, ARG_time, ARG_repeat };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,     MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_time,   MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_repeat, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(args), allowed_args, args);

    rtc_alarm_check_id(args[ARG_alarm_id].u_int);
    if (machine_rtc_source == 0) {
        mp_raise_OSError(MP_EIO);
    }

    uint32_t target;
    uint32_t period = 0;

    if (mp_obj_is_type(args[ARG_time].u_obj, &mp_type_tuple)
        || mp_obj_is_type(args[ARG_time].u_obj, &mp_type_list)) {
        if (args[ARG_repeat].u_bool) {
            mp_raise_ValueError(MP_ERROR_TEXT("repeat needs a millisecond interval"));
        }
        /* An absolute datetime, in the same 8-tuple layout as RTC.datetime().
         * The counter already holds seconds since 2000-01-01, which is this
         * port's epoch, so the conversion is the whole of the work. */
        mp_obj_t *items;
        mp_obj_get_array_fixed_n(args[ARG_time].u_obj, 8, &items);
        target = timeutils_seconds_since_2000(
            mp_obj_get_int(items[0]), mp_obj_get_int(items[1]), mp_obj_get_int(items[2]),
            mp_obj_get_int(items[4]), mp_obj_get_int(items[5]), mp_obj_get_int(items[6]));
        if ((target - RTC_GetCounter()) > 0x80000000u) {
            mp_raise_ValueError(MP_ERROR_TEXT("alarm time is in the past"));
        }
    } else {
        mp_int_t ms = mp_obj_get_int(args[ARG_time].u_obj);
        if (ms <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("alarm time must be positive"));
        }
        /* Round up: this clock has no fraction of a second to offer, and an
         * alarm that fires early is worse than one that fires late. */
        uint32_t seconds = ((uint32_t)ms + 999) / 1000;
        target = RTC_GetCounter() + seconds;
        if (args[ARG_repeat].u_bool) {
            period = seconds;
        }
    }

    machine_rtc_alarm_period = period;
    if (!rtc_alarm_arm_at(target)) {
        machine_rtc_alarm_period = 0;
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_alarm_obj, 1, machine_rtc_alarm);

// RTC.alarm_left(alarm_id=0) -- milliseconds, always a whole second's worth.
static mp_obj_t machine_rtc_alarm_left(size_t n_args, const mp_obj_t *args) {
    rtc_alarm_check_id(n_args > 1 ? mp_obj_get_int(args[1]) : 0);
    return mp_obj_new_int_from_uint(rtc_alarm_left_seconds() * 1000u);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_rtc_alarm_left_obj, 1, 2, machine_rtc_alarm_left);

// RTC.alarm_cancel(alarm_id=0)
static mp_obj_t machine_rtc_alarm_cancel(size_t n_args, const mp_obj_t *args) {
    rtc_alarm_check_id(n_args > 1 ? mp_obj_get_int(args[1]) : 0);
    machine_rtc_alarm_clear();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_rtc_alarm_cancel_obj, 1, 2, machine_rtc_alarm_cancel);

/* irq.trigger(ms) re-arms as a repeating alarm, and irq.trigger(0) cancels,
 * which is the convention the other ports with an RTC irq object follow. */
static mp_uint_t machine_rtc_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    (void)self_in;
    if (new_trigger == 0) {
        machine_rtc_alarm_clear();
    } else {
        uint32_t seconds = ((uint32_t)new_trigger + 999) / 1000;
        machine_rtc_alarm_period = seconds;
        if (!rtc_alarm_arm_at(RTC_GetCounter() + seconds)) {
            machine_rtc_alarm_period = 0;
            mp_raise_OSError(MP_EIO);
        }
    }
    return 0;
}

static mp_uint_t machine_rtc_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    (void)self_in;
    if (info_type == MP_IRQ_INFO_FLAGS) {
        return machine_rtc_alarm_armed ? 1 : 0;
    }
    return 0;   // MP_IRQ_INFO_TRIGGERS: ALARM0 is 0
}

static const mp_irq_methods_t machine_rtc_irq_methods = {
    .trigger = machine_rtc_irq_trigger,
    .info = machine_rtc_irq_info,
};

// RTC.irq(*, trigger=RTC.ALARM0, handler=None, wake=machine.IDLE, hard=False)
static mp_obj_t machine_rtc_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_trigger, ARG_handler, ARG_wake, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_trigger, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_handler, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_wake,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_hard,    MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(args), allowed_args, args);

    rtc_alarm_check_id(args[ARG_trigger].u_int);
    /* wake is accepted and ignored on purpose: the alarm is routed through
     * EXTI line 17 whenever it is armed, so it already wakes the chip from
     * every sleep mode this port offers. There is nothing to select. */

    mp_irq_obj_t *irq = MP_STATE_PORT(machine_rtc_irq_object);
    if (irq == NULL) {
        irq = mp_irq_new(&machine_rtc_irq_methods, MP_OBJ_FROM_PTR(&machine_rtc_singleton));
        MP_STATE_PORT(machine_rtc_irq_object) = irq;
    }
    irq->handler = args[ARG_handler].u_obj;
    irq->ishard = args[ARG_hard].u_bool;
    return MP_OBJ_FROM_PTR(irq);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_irq_obj, 1, machine_rtc_irq);

enum { ARG_id, ARG_source, ARG_datetime };
static const mp_arg_t machine_rtc_allowed_args[] = {
    { MP_QSTR_id,       MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_source,   MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    { MP_QSTR_datetime, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
};

static mp_obj_t machine_rtc_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    (void)type;
    mp_arg_val_t args[MP_ARRAY_SIZE(machine_rtc_allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(machine_rtc_allowed_args), machine_rtc_allowed_args, args);

    if (args[ARG_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("only RTC(0) exists"));
    }

    if (args[ARG_source].u_int >= 0) {
        uint8_t source = (uint8_t)args[ARG_source].u_int;
        if (source < MACHINE_RTC_SRC_LSE || source > MACHINE_RTC_SRC_HSE) {
            mp_raise_ValueError(MP_ERROR_TEXT("source must be RTC.LSE, RTC.LSI or RTC.HSE"));
        }
        if (!machine_rtc_configure(source)) {
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("RTC oscillator did not start"));
        }
    }

    if (args[ARG_datetime].u_obj != mp_const_none) {
        if (machine_rtc_source == 0) {
            mp_raise_OSError(MP_EIO);
        }
        machine_rtc_set_datetime(args[ARG_datetime].u_obj);
    }

    return MP_OBJ_FROM_PTR(&machine_rtc_singleton);
}

static const mp_rom_map_elem_t machine_rtc_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_datetime), MP_ROM_PTR(&machine_rtc_datetime_obj) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_rtc_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_now), MP_ROM_PTR(&machine_rtc_now_obj) },
    { MP_ROM_QSTR(MP_QSTR_source), MP_ROM_PTR(&machine_rtc_source_obj) },

    { MP_ROM_QSTR(MP_QSTR_alarm), MP_ROM_PTR(&machine_rtc_alarm_obj) },
    { MP_ROM_QSTR(MP_QSTR_alarm_left), MP_ROM_PTR(&machine_rtc_alarm_left_obj) },
    { MP_ROM_QSTR(MP_QSTR_alarm_cancel), MP_ROM_PTR(&machine_rtc_alarm_cancel_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq), MP_ROM_PTR(&machine_rtc_irq_obj) },

    { MP_ROM_QSTR(MP_QSTR_ALARM0), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_LSE), MP_ROM_INT(MACHINE_RTC_SRC_LSE) },
    { MP_ROM_QSTR(MP_QSTR_LSI), MP_ROM_INT(MACHINE_RTC_SRC_LSI) },
    { MP_ROM_QSTR(MP_QSTR_HSE), MP_ROM_INT(MACHINE_RTC_SRC_HSE) },
};
static MP_DEFINE_CONST_DICT(machine_rtc_locals_dict, machine_rtc_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_rtc_type,
    MP_QSTR_RTC,
    MP_TYPE_FLAG_NONE,
    make_new, machine_rtc_make_new,
    print, machine_rtc_print,
    locals_dict, &machine_rtc_locals_dict
    );

/* void * rather than mp_irq_obj_t *: the declaration is copied verbatim into
 * genhdr/root_pointers.h, which every translation unit includes and which has
 * no mpirq.h in scope. Every other port with an irq root pointer does the
 * same. */
MP_REGISTER_ROOT_POINTER(void *machine_rtc_irq_object);
