// ARM64 guest instructions running on a bionic thread. The caller owns guest
// TLS, safe-point selection and validation of the saved PC/SP before applying.
#pragma once
#include "dm_uc.h"
#include <string.h>

typedef struct {
    uint64_t x[31], sp, pc, nzcv;
    uint32_t fpsr, fpcr;
    __uint128_t v[32];
} dm_checkpoint_cpu;

static inline int dm_checkpoint_cpu_transfer(ucontext_t *uc, dm_checkpoint_cpu *saved, int restore) {
    struct fpsimd_context *fp = dm_uc_record(uc, FPSIMD_MAGIC);
    if (!fp || fp->head.size != sizeof *fp) return -1;
    struct sigcontext *cpu = &uc->uc_mcontext;
    const uint64_t mask = UINT64_C(0xf0000000);
    if (restore) {
        uint64_t native_x18 = cpu->regs[18];
        memcpy(cpu->regs, saved->x, sizeof saved->x);
        cpu->regs[18] = native_x18;
        cpu->sp = saved->sp;
        cpu->pc = saved->pc;
        cpu->pstate = (cpu->pstate & ~mask) | (saved->nzcv & mask);
        fp->fpsr = saved->fpsr;
        fp->fpcr = saved->fpcr;
        memcpy(fp->vregs, saved->v, sizeof saved->v);
    } else {
        memcpy(saved->x, cpu->regs, sizeof saved->x);
        saved->x[18] = 0;  // logical x18 is in the runtime's TLS slots
        saved->sp = cpu->sp;
        saved->pc = cpu->pc;
        saved->nzcv = cpu->pstate & mask;
        saved->fpsr = fp->fpsr;
        saved->fpcr = fp->fpcr;
        memcpy(saved->v, fp->vregs, sizeof saved->v);
    }
    return 0;
}
