    .text
    .p2align 2
    .global probe_guest_enter
    .type probe_guest_enter, %function
probe_guest_enter:
    mov x16, x0
    mov sp, x1
    mov x0, x2
    mov x30, xzr
    br x16
    .size probe_guest_enter, .-probe_guest_enter

// This body is copied to the guest's address range. Only local branches and
// addresses from its guest mailbox are used, so no host address goes in its live state.
    .global probe_guest_start
    .global probe_guest_wait
    .global probe_guest_wait_end
    .global probe_guest_end
probe_guest_start:
    sub sp, sp, #16
    ldr x19, [x0]
    add x19, x19, #37
    lsl x20, x19, #1
    str x20, [sp]
    movi v8.16b, #0x5a
    cmp x19, x19
    ldr x23, [x0, #48]
    add x15, x0, #8
    add x14, x0, #12
    mov w9, #1
    stlr w9, [x15]
probe_guest_wait:
    ldar w10, [x14]
    cbz w10, probe_guest_wait
probe_guest_wait_end:
    str x19, [x0, #16]
    ldr x21, [sp]
    str x21, [x0, #24]
    umov x22, v8.d[1]
    str x22, [x0, #32]
    cset w21, eq
    str w21, [x0, #40]
    ldr w24, [x23]
    str w24, [x0, #56]
    add x10, x0, #44
    stlr w9, [x10]
    mov x8, #93  // __NR_exit on AArch64: exit this worker, not the probe's host
    mov x0, xzr
    svc #0
    brk #0
probe_guest_end:
    .section .note.GNU-stack,"",%progbits
