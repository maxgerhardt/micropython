/* machine.lightsleep() and machine.deepsleep() for the CH32H417.
 *
 * The chip offers exactly two low-power modes -- there is no standby on this
 * part, whatever the stray "Standby" labels in the reference manual's RTC
 * diagram suggest -- so the mapping is forced and simple:
 *
 *   lightsleep -> Sleep. SLEEPDEEP=0, WFI. Core clock off, every peripheral
 *                 clock still running, execution resumes in place.
 *   deepsleep  -> Stop.  SLEEPDEEP=1, WFI. Every clock off, then reset.
 *
 * Two things about Stop mode on this SoC drive the whole design.
 *
 * **It takes both cores.** RM 2.3: "The stop mode only takes effect when both
 * V3F and V5F enter stop mode." That is already half true at all times -- the
 * V3F boot stub parks inside PWR_EnterSTOPMode(), which sets its SLEEPDEEP and
 * waits, so it is permanently requesting Stop. This file supplies the other
 * half. The corollary is a hazard: SLEEPDEEP must never be left set on the
 * V5F, or the next ordinary idle WFI -- in mp_hal_ch32_wfe(), or inside
 * mp_hal_delay_ms() -- would stop the entire chip instead of just the core.
 * lightsleep() clears it defensively for that reason, and deepsleep() only
 * ever exits through a reset.
 *
 * **Waking leaves the clock tree in ruins.** RM 2.3.3: HSE, HSI and every PLL
 * are disabled, and "the HSI is called the default system clock" afterwards.
 * Rebuilding it from here is not possible: the 786-line clock setup lives in
 * system_ch32h417_v3f.c and the V5F image carries only the 139-line variant
 * that refreshes cached values. Rather than duplicate it, deepsleep honours
 * MicroPython's contract that it never returns, and issues a system reset on
 * wake. The V3F stub then reboots and configures every PLL exactly as it does
 * at power-on. The clock restoration falls out of the API semantics for free.
 *
 * Wake sources, and why EXTI is not optional for the ones that use it: in Stop
 * mode the core and its interrupt controller are clock-gated, so a peripheral
 * interrupt cannot reach them. EXTI sits in the always-on domain and is what
 * restarts the clocks. RM 2.3.4 spells this out for the RTC -- "the external
 * break line 17 needs to be configured". Which source is used for a timed
 * sleep, and why it is not the obvious one, is argued above
 * machine_sleep_deep().
 */

#include "ch32h417.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "irq.h"
#include "machine_rtc.h"
#include "machine_wdt.h"
#include "machine_sleep.h"

/* NVIC->SCTLR. Bit 2 selects deep sleep; bit 3 selects WFE-vs-WFI semantics
 * for the `wfi` instruction, which is the only one the core has. Note that
 * __WFI() clears bit 3 and leaves bit 2 alone -- it is not a way to guarantee
 * a shallow sleep. */
#define SCTLR_SLEEPDEEP (1u << 2)

#define EXTI_LINE_RTC_ALARM (1u << 17)

/* The IWDG covers short sleeps with millisecond resolution; past its ceiling
 * the RTC alarm takes over at one-second granularity. See the block comment
 * above machine_sleep_deep() for why it is not LPTIM. */
#define IWDG_MAX_SLEEP_MS (25000u)

/* Survives a reset and is lost when power goes, because it lives in the same
 * NOLOAD section as machine.mem_backup() and nothing copies or clears that.
 * Those are exactly the semantics wanted: waking from deepsleep must report
 * DEEPSLEEP_RESET, while a power cycle must still report PWRON_RESET. This
 * chip has no backup registers to use instead -- see the comment at the top of
 * machine_mem_backup.c for the evidence that they simply do not exist. */
#define MACHINE_SLEEP_FLAG_MAGIC (0x1D5EE9A7)

__attribute__((section(".mem_backup"), aligned(4)))
static volatile uint32_t machine_sleep_flag;

bool machine_sleep_deepsleep_flag_take(void) {
    bool woke = (machine_sleep_flag == MACHINE_SLEEP_FLAG_MAGIC);
    machine_sleep_flag = 0;
    return woke;
}

/* Arm the RTC alarm. The EXTI routing that makes this able to wake Stop --
 * line 17, which is not clock-gated and is what restarts the clocks, RM 2.3.4
 * -- now lives in machine_rtc_alarm_in(), so that an alarm armed from Python
 * wakes the chip exactly as one armed here does. The handler is there too.
 *
 * Sleeping past the LPTIM ceiling therefore replaces any alarm the program had
 * pending: there is one comparator and this takes it. */
static bool rtc_alarm_arm_ms(uint32_t ms) {
    return machine_rtc_alarm_in((ms + 999) / 1000);
}

/* --- the two entry points --- */

void machine_sleep_light(mp_int_t ms) {
    /* Shallow, explicitly. If SLEEPDEEP were left set from anywhere, this
     * would stop the whole chip -- the V3F is always requesting Stop -- and
     * lightsleep() would silently become deepsleep() without the reset that
     * makes deepsleep survivable. */
    NVIC->SCTLR &= ~SCTLR_SLEEPDEEP;

    if (ms < 0) {
        __WFI();
        return;
    }

    /* Wake early for anything scheduled, the same as time.sleep_ms(): the
     * 1 ms SysTick means the core is woken often regardless, and in Sleep mode
     * every peripheral clock is running anyway, so there is nothing to gain by
     * refusing to service USB for the duration. */
    mp_hal_delay_ms((mp_uint_t)ms);
}

/* Why the wake source is an IWDG reset and not LPTIM.
 *
 * The reference manual's EXTI map (table 4-12) lists "EXTI23 LPTIM1 wakeup
 * event", and LPTIM is the obvious choice: it runs from the LSE in Stop
 * (17.3.13) and would give 30 us resolution against the RTC alarm's one
 * second. It does not work. On this silicon an LPTIM1 ARRM -- or CMPM, or
 * every IER bit set at once -- never asserts EXTI line 23, with LSE or LSI, at
 * any prescaler, with AFIO and PWR clocked, configured exactly as WCH's own
 * LPTIM_LP_WakeUp example does. Verified against a software trigger through
 * SWIEVR, which does set the flag on line 23, so the base address, offsets and
 * masks are all correct; and against the RTC alarm, which does drive line 17.
 * Confirmed from inside Stop, not merely inferred from the awake test: with
 * LPTIM1 armed for 2 s and an IWDG backstop at 8 s, the board came back after
 * 7.94 s and a marker in retained RAM showed it had never returned from
 * PWR_EnterSTOPMode(). The LPTIM wake simply does not happen.
 *
 * The manual does say so, just not in the LPTIM chapter. 17.3.13 claims "the
 * LPTIM peripheral is active when it is timed by LSE or LSI, and the LPTIM
 * interrupt causes the device to exit stop mode", but 2.3.3 lists what
 * actually survives Stop -- "Independent Watchdog Dog (IWDG), Real Time Clock
 * (RTC), Low Frequency Clock (LSI/LSE)" -- and LPTIM is not in it. Where the
 * two disagree, the mode chapter is the one that matches the silicon.
 *
 * So both wake sources here are ones that were checked first. Table 2-1 lists
 * exactly three ways out of Stop: an external interrupt or event, NRST, and an
 * IWDG reset. The IWDG needs no EXTI at all, and since deepsleep resets on
 * wake regardless, its reset *is* the wake -- it is not a workaround, it is
 * the natural fit, and it brings millisecond resolution with it. Past its
 * ~26 s ceiling the RTC alarm takes over, at one-second granularity, on the
 * EXTI line the manual documents. */
MP_NORETURN void machine_sleep_deep(mp_int_t ms) {
    if (ms >= 0) {
        uint32_t want = (uint32_t)ms;
        bool armed = false;

        /* Not if the application already has a watchdog running: the IWDG
         * cannot be stopped, retimed downwards, or shared, and quietly
         * repurposing it would disarm the caller's safety net. */
        if (want <= IWDG_MAX_SLEEP_MS && !machine_wdt_is_running()) {
            armed = machine_wdt_start_raw(want);
        }
        if (!armed) {
            armed = rtc_alarm_arm_ms(want);
        }
        if (!armed) {
            /* Refusing to sleep is the only safe answer: with no wake source
             * the chip stops until NRST or a power cycle, and a caller who
             * asked for a timed sleep has no reason to expect that. */
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no usable wake timer"));
        }
    }

    /* Recorded before sleeping, not after: the wake path is a reset, so there
     * is no "after" in which to set it. */
    machine_sleep_flag = MACHINE_SLEEP_FLAG_MAGIC;

    /* Push the last of the console out before the clocks stop, so a session
     * does not end on half a line. */
    mp_hal_stdout_tx_strn("\r\n", 2);

    RCC_HB1PeriphClockCmd(RCC_HB1Periph_PWR, ENABLE);
    (void)RCC->HB1PCENR;

    /* PWR_Regulator_LowPower sets LPDS, which drops the voltage regulator into
     * its low-power mode for the duration rather than leaving it running
     * normally with every clock already stopped (RM 2.3.3). It costs a longer
     * wake while the regulator recovers, which is free here because waking is
     * a reset either way. PWR is one shared peripheral, so this write is the
     * one that counts even though the V3F reached its own Stop request first.
     *
     * The SDK call rather than a hand-rolled SLEEPDEEP+WFI: it sets LPDS,
     * sets SLEEPDEEP, sleeps, and clears SLEEPDEEP again on the way out, which
     * matters because a stray SLEEPDEEP would turn the next ordinary idle WFI
     * into another chip-wide stop.
     *
     * The V3F is already parked in its own PWR_EnterSTOPMode(), so this is the
     * call that actually stops the chip. */
    PWR_EnterSTOPMode(PWR_Regulator_LowPower, PWR_STOPEntry_WFI);

    /* Only reached on an EXTI wake; an IWDG wake arrives as a reset instead.
     * The clocks are back but the system is on HSI with every PLL off, so do
     * as little as possible before resetting -- anything that assumes the
     * configured clock tree would misbehave. PWR_EnterSTOPMode() has already
     * cleared SLEEPDEEP. */
    NVIC_SystemReset();
    for (;;) {
    }
}
