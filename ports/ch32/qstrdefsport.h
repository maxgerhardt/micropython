// Port-specific qstrs.
//
// These four name the machine.reset_cause() constants. They are declared here
// rather than found by the automatic scan because they only ever appear inside
// MICROPY_PY_MACHINE_EXTRA_GLOBALS, which this port defines in modmachine.c
// and extmod/modmachine.c expands -- and the qstr extraction pass does not
// follow them across that boundary.
Q(PWRON_RESET)
Q(HARD_RESET)
Q(WDT_RESET)
Q(SOFT_RESET)
