// Restore a field capture in a fresh host. The standalone test checks one
// guest return. The NativeActivity can resume an app-private field session.
#include "dm.h"
#include "gfx_vk.h"
#include "nvn_count.h"
#include "checkpoint_memory.h"
#include "checkpoint_modules.h"
#include "checkpoint_pools.h"
#include "checkpoint_wait.h"
#include "checkpoint_image.h"
#include "survey_threads.h"
#include <errno.h>
#include <fcntl.h>
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

static int check_cached_nvn_call(void) {
    extern const char *nvn_names[NVN_NFUNC];
    unsigned index = 0;
    while (index < NVN_NFUNC && strcmp(nvn_names[index], "nvnDeviceBuilderSetFlags")) ++index;
    const void *target = dm_checkpoint_nvn_slot(index);
    if (!target) return -ENOENT;
    for (int i = 0; i < g_nmod; ++i) {
        module_t *m = &g_mod[i];
        for (size_t off = m->data_off; off + sizeof(void *) <= m->key_page_off; off += sizeof(void *)) {
            void (*cached)(void *, int);
            memcpy(&cached, m->base + off, sizeof cached);
            if ((const void *)cached != target) continue;
            uint32_t flags = 0;
            cached(&flags, 0x61);
            if (flags != 0x61) return -EIO;
            LOG("[snapshot] cold NVN call through restored %s+%#zx succeeded", m->name, off);
            return 0;
        }
    }
    return -ENOENT;
}

// Only an internally captured, app-private sample may run. At this point
// every writeable filesystem mount points at a disposable transaction.
static pthread_mutex_t roster_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t roster_changed = PTHREAD_COND_INITIALIZER;
static dm_checkpoint_wait_cpu audio_worker;
static unsigned roster_ready;
static int roster_go;

static void roster_signal(int sig, siginfo_t *si, void *context) {
    (void)sig; (void)si;
    if (!stepping || dm_checkpoint_cpu_transfer(context, &stepping->cpu, 1)) _exit(73);
}

static void *run_field_thread(void *arg) {
    dm_checkpoint_wait_cpu *cpu = arg;
    int dm_checkpoint_attach_audio(dm_checkpoint_wait_cpu *);
    int service = cpu == &audio_worker;
    int rc = service ? dm_checkpoint_attach_audio(cpu) : dm_checkpoint_attach_handle(cpu->handle);
    if (rc) { LOG("[snapshot] field handle %#x refused (%d)", cpu->handle, rc); _exit(74); }
    stepping = cpu;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL)) _exit(75);
    pthread_mutex_lock(&roster_lock);
    ++roster_ready;
    pthread_cond_broadcast(&roster_changed);
    while (!roster_go) pthread_cond_wait(&roster_changed, &roster_lock);
    pthread_mutex_unlock(&roster_lock);
    if (service) {
        void *dm_checkpoint_audio_function(void);
        cpu->cpu.pc = (uintptr_t)dm_checkpoint_audio_function();
    } else rc = cpu->svc == DM_CHECKPOINT_RETRY_CLOCK ? dm_checkpoint_clock_cpu_finish(cpu) : dm_checkpoint_wait_cpu_finish(cpu);
    if (rc) { LOG("[snapshot] field continuation %#x refused (%d)", cpu->handle, rc); _exit(76); }
    raise(SIGUSR1);
    _exit(77);
}

static int run_field(survey_roster *all, int interactive) {
    struct sigaction action = {.sa_sigaction = roster_signal, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) return -errno;
    unsigned count = all->count + !!audio_worker.handle;
    for (unsigned i = 0; i < count; ++i) {
        pthread_t t;
        dm_checkpoint_wait_cpu *cpu = i == all->count ? &audio_worker : &all->cpus[i];
        int rc = pthread_create(&t, NULL, run_field_thread, cpu);
        if (rc) { LOG("[snapshot] launch failed for %#x (%d)", cpu->handle, rc); _exit(78); }
        pthread_detach(t);
    }
    pthread_mutex_lock(&roster_lock);
    while (roster_ready < count) pthread_cond_wait(&roster_changed, &roster_lock);
    uint64_t start = dm_nvn_frame_count();
    LOG("[snapshot] releasing %u restored field threads, initial presents=%llu", roster_ready, (unsigned long long)start);
    roster_go = 1;
    pthread_cond_broadcast(&roster_changed);
    pthread_mutex_unlock(&roster_lock);
    if (interactive) {
        LOG("[snapshot] resumed playable field session");
        for (;;) pause(); // The Android lifecycle owns this process.
    }
    for (int i = 0; i < 20; ++i) {
        sleep(1);
        uint64_t frames = dm_nvn_frame_count() - start;
        LOG("[snapshot] field trial second=%d new presents=%llu", i+1, (unsigned long long)frames);
        if (frames >= 120) return 0;
    }
    void svc_thread_dump(void);
    svc_thread_dump();
    return -ETIMEDOUT;
}

static int replay_scene(const char *module_dir, const char *section_dir,
                        void (*load)(const char *, const char **, int), int trial) {
    const char *names[] = {"modules", "memory", "pools", "bindings", "events", "threads", "images", "handles", "files"};
    int fd[9];
    int dir = open(section_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) { perror("open scene directory"); return 2; }
    for (unsigned i = 0; i < 9; ++i) {
        fd[i] = openat(dir, names[i], O_RDONLY | O_NOFOLLOW);
        if (fd[i] < 0) { perror(names[i]); return 2; }
    }
    const char *mods[] = DM_GAME_MODULES;
    int rc = dm_checkpoint_modules_begin(module_dir, mods, DM_GAME_NMOD, fd[0]);
    if (!rc) {
        load(module_dir, mods, DM_GAME_NMOD);
        rc = dm_checkpoint_modules_baseline();
    }
    if (!rc) rc = dm_checkpoint_memory_restore(fd[1]);
    if (rc) { fprintf(stderr, "scene guest address space refused: %d\n", rc); return 1; }
    if ((rc = dm_checkpoint_modules_restore(fd[0]))) {
        fprintf(stderr, "scene module data refused: %d\n", rc); return 1;
    }
    int dm_checkpoint_files(int, int);
    if ((rc = dm_checkpoint_files(fd[8], 1))) {
        fprintf(stderr, "scene read-only asset handles refused: %d\n", rc); return 1;
    }
    if ((rc = check_cached_nvn_call())) {
        fprintf(stderr, "restored NVN function pointer refused: %d\n", rc); return 1;
    }
    vk_init();
    if ((rc = dm_checkpoint_pools_restore(fd[2]))) {
        fprintf(stderr, "scene GPU pools refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_bindings_restore(fd[3]))) {
        fprintf(stderr, "scene descriptor table refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_events_restore(fd[4]))) {
        fprintf(stderr, "scene event table refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_handles_restore(fd[7]))) {
        fprintf(stderr, "scene thread handle table refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_scene_images(fd[6], 1))) {
        fprintf(stderr, "scene image storage refused: %d\n", rc); return 1;
    }
    static survey_roster all;
    dm_checkpoint_wait_cpu selected;
    if (trial) {
        int mounts = openat(dir, "mounts", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        int dm_checkpoint_mounts(int, const char *, int);
        rc = mounts < 0 ? -errno : dm_checkpoint_mounts(mounts, section_dir, 1);
        if (mounts >= 0) close(mounts);
        if (rc) { LOG("[snapshot] field mounts refused (%d)", rc); return 1; }
    }
    if ((rc = survey_validate_threads(fd[5], &selected, trial ? &all : NULL))) {
        LOG("[snapshot] scene thread continuations refused: %d", rc); return 1;
    }
    if (trial) {
        int audio = openat(dir, "audio", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        int dm_checkpoint_audio_outputs(int, int);
        rc = audio < 0 ? -errno : dm_checkpoint_audio_outputs(audio, 1);
        if (audio >= 0) close(audio);
        if (rc) { LOG("[snapshot] audio outputs refused (%d)", rc); return 1; }
        int timeline = openat(dir, "timeline", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        int dm_checkpoint_graphics_timeline(int, int);
        rc = timeline < 0 ? -errno : dm_checkpoint_graphics_timeline(timeline, 1);
        if (timeline >= 0) close(timeline);
        if (rc) { LOG("[snapshot] graphics timeline refused (%d)", rc); return 1; }
        int renderer = openat(dir, "renderer", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        int dm_checkpoint_renderer(int, int, dm_checkpoint_wait_cpu *);
        rc = renderer < 0 ? -errno : dm_checkpoint_renderer(renderer, 1, &audio_worker);
        if (renderer >= 0) close(renderer);
        if (rc) { LOG("[snapshot] renderer state refused (%d)", rc); return 1; }
        for (unsigned i = 0; i < sizeof fd / sizeof fd[0]; ++i) close(fd[i]);
        close(dir);
        return run_field(&all, trial == 2);
    }
    if ((rc = verify_field_return(&selected))) {
        LOG("[snapshot] cold field wait return refused: %d", rc);
        return 1;
    }
    LOG("[snapshot] cold PID %d validated field resources", getpid());
    puts("Cold host rebuilt field memory, modules, GPU pools, descriptor tables and event bindings.");
    return 0;
}

int svc_replay_scene_core(const char *mods, const char *dir, void (*load)(const char *, const char **, int)) {
    return replay_scene(mods, dir, load, 0);
}
int svc_replay_private_scene(const char *mods, const char *dir, void (*load)(const char *, const char **, int)) {
    return replay_scene(mods, dir, load, 1);
}

int svc_resume_private_scene(const char *mods, const char *dir, void (*load)(const char *, const char **, int)) {
    return replay_scene(mods, dir, load, 2);
}
