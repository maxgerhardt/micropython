// Port-specific qstrs.
// *FORMAT-OFF*
//
// The FORMAT-OFF above is required, not decorative: uncrustify parses this as
// C and rewrites Q(/lib) into Q(/ lib), which would define a qstr with a space
// in it. ports/rp2/qstrdefsport.h carries the same marker for the same reason.
//
// main.c appends /lib to sys.path. It has to be declared here because the
// automatic scan turns MP_QSTR__slash_lib into a qstr whose *text* is
// "_slash_lib" -- nothing tells it those underscores stand for a slash. Without
// this, sys.path held a directory name no filesystem has, and
// `mpremote mip install` wrote to /lib where nothing could ever import it.
Q(/lib)
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
