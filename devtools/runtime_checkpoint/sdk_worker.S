// Copy this position-independent caller into probe-owned executable memory at
// the same address in each process. The function it invokes is original SDK
// code. No Android return address is needed after switching guest stacks.
.text
.p2align 2
.global sdk_caller_begin
.global sdk_caller_end
sdk_caller_begin:
    mov x19, x0
    ldp x16, x2, [x19]
    ldr x3, [x19, #16]
    add x0, x19, #32
    mov x1, #32
    mov w9, #1
    add x10, x19, #24
    stlr w9, [x10]
    blr x16
    mov w9, #1
    add x10, x19, #28
    stlr w9, [x10]
1:  wfe
    b 1b
.global sdk_cv_caller
sdk_cv_caller:
    mov x19, x0
    ldp x16, x0, [x19]
    ldp x1, x2, [x19, #16]
    mov x3, #-1
    blr x16
    str x0, [x19, #32]
    add x10, x19, #40
    mov x9, #1
    stlr x9, [x10]
2:  wfe
    b 2b
sdk_caller_end:
.section .note.GNU-stack,"",%progbits
