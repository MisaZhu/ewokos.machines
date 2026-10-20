/* Minimal Q8B kernel: stop at the very first instruction.
 * Used to decide whether the EDL/900e transition is caused by kernel code
 * or by the stub/ExitBootServices/PMIC watchdog path. */
.section .text
.global __entry
__entry:
    wfi
    b __entry
