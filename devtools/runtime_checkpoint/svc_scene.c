// Standalone probe of the first guest return in a restored scene.
#include "dm.h"
#include "checkpoint_wait.h"
#include "checkpoint/restore.h"
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

// After an original SDK wait leaf's RET, stop on the first instruction
// fetch of the original caller. No guest code or registers are patched.
// The two handlers live only in the single-worker diagnostic process.
static _Thread_local dm_checkpoint_wait_cpu *stepping;
static _Thread_local sigjmp_buf bootstrap;
static void step_signal(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (!stepping) _exit(70);
    if (sig == SIGUSR1) {
        if (dm_checkpoint_cpu_transfer(uc, &stepping->cpu, 1)) _exit(71);
        return;
    }
    if (info->si_code != SEGV_ACCERR || (uintptr_t)info->si_addr != stepping->cpu.x[30] ||
        uc->uc_mcontext.pc != stepping->cpu.x[30] || uc->uc_mcontext.sp != stepping->cpu.sp ||
        uc->uc_mcontext.regs[0] != 0 || uc->uc_mcontext.regs[29] != stepping->cpu.x[29]) _exit(72);
    siglongjmp(bootstrap, 1);
}
static void *step_thread(void *p) {
    dm_checkpoint_wait_cpu *cpu = p;
    int rc = dm_checkpoint_attach_handle(cpu->handle);
    if (!rc) rc = dm_checkpoint_wait_cpu_finish(cpu);
    if (rc || cpu->cpu.x[0]) {
        LOG("[snapshot] field thread %#x attach/wait rc=%d SVC result=%#llx",
            cpu->handle, rc, (unsigned long long)cpu->cpu.x[0]);
        return (void *)(intptr_t)(rc ? rc : -EIO);
    }
    // A NativeActivity worker can inherit the Java runtime's blocked signal
    // mask. Deliver the probe signals only on this disposable pthread.
    sigset_t signals, previous;
    sigemptyset(&signals);
    sigaddset(&signals, SIGUSR1);
    sigaddset(&signals, SIGSEGV);
    rc = pthread_sigmask(SIG_UNBLOCK, &signals, &previous);
    if (rc) return (void *)(intptr_t)-rc;
    stepping = cpu;
    if (!sigsetjmp(bootstrap, 1)) {
        raise(SIGUSR1);
        LOG("[snapshot] field thread's CPU transfer unexpectedly returned");
        rc = -EIO;
    }
    stepping = NULL;
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return (void *)(intptr_t)rc;
}
static int verify_field_return(dm_checkpoint_wait_cpu *cpu) {
    size_t size = (size_t)getpagesize();
    void *page = (void *)(uintptr_t)(cpu->cpu.x[30] & ~(size - 1));
    struct sigaction action = {.sa_sigaction = step_signal, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    struct sigaction old_enter, old_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &old_enter)) return -errno;
    if (sigaction(SIGSEGV, &action, &old_stop)) {
        int rc = -errno; sigaction(SIGUSR1, &old_enter, NULL); return rc;
    }
    int rc = 0;
    if (mprotect(page, size, PROT_READ)) rc = -errno;
    else {
        pthread_t worker;
        void *result = NULL;
        rc = -pthread_create(&worker, NULL, step_thread, cpu);
        if (!rc) rc = -pthread_join(worker, &result);
        if (!rc) rc = (int)(intptr_t)result;
        if (mprotect(page, size, PROT_READ | PROT_EXEC)) rc = -errno;
    }
    sigaction(SIGSEGV, &old_stop, NULL);
    sigaction(SIGUSR1, &old_enter, NULL);
    if (!rc) {
        module_t *m = module_of((void *)(uintptr_t)cpu->cpu.x[30]);
        LOG("[snapshot] cold field thread %#x executed SDK RET to %s+%#lx with the saved SP and FP",
            cpu->handle, m->name, (unsigned long)(cpu->cpu.x[30] - (uintptr_t)m->base));
    }
    return rc;
}

int svc_replay_scene_core(const char *mods, const char *dir, void (*load)(const char *, const char **, int)) {
    return dm_checkpoint_replay_scene(mods, dir, load, CHECKPOINT_PROBE_RETURN, verify_field_return);
}
