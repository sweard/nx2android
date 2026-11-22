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
#include "restore.h"
#include "services.h"
#include "sections.h"
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

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
static _Thread_local dm_checkpoint_wait_cpu *resuming_thread;
static dm_checkpoint_wait_cpu audio_worker;
static unsigned roster_ready;
static int roster_go;

static void roster_signal(int sig, siginfo_t *si, void *context) {
    (void)sig; (void)si;
    if (!resuming_thread || dm_checkpoint_cpu_transfer(context, &resuming_thread->cpu, 1)) _exit(73);
}

static void *run_field_thread(void *arg) {
    dm_checkpoint_wait_cpu *cpu = arg;
    int service = cpu == &audio_worker;
    int rc = service ? dm_checkpoint_attach_audio(cpu) : dm_checkpoint_attach_handle(cpu->handle);
    if (rc) { LOG("[snapshot] field handle %#x refused (%d)", cpu->handle, rc); _exit(74); }
    resuming_thread = cpu;
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

static int restore_resources(const char *module_dir, const char *section_dir, checkpoint_loader load,
                             enum checkpoint_replay_mode mode, const int *fd,
                             survey_roster *all, dm_checkpoint_wait_cpu *selected) {
    const char *mods[] = DM_GAME_MODULES;
    int rc = dm_checkpoint_modules_begin(module_dir, mods, DM_GAME_NMOD, fd[SECTION_MODULES]);
    if (!rc) {
        load(module_dir, mods, DM_GAME_NMOD);
        rc = dm_checkpoint_modules_baseline();
    }
    if (!rc) rc = dm_checkpoint_memory_restore(fd[SECTION_MEMORY]);
    if (rc) { fprintf(stderr, "scene guest address space refused: %d\n", rc); return 1; }
    if ((rc = dm_checkpoint_modules_restore(fd[SECTION_MODULES]))) {
        fprintf(stderr, "scene module data refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_files(fd[SECTION_FILES], 1))) {
        fprintf(stderr, "scene read-only asset handles refused: %d\n", rc); return 1;
    }
    if ((rc = check_cached_nvn_call())) {
        fprintf(stderr, "restored NVN function pointer refused: %d\n", rc); return 1;
    }
    vk_init();
    if ((rc = dm_checkpoint_pools_restore(fd[SECTION_POOLS]))) {
        fprintf(stderr, "scene GPU pools refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_bindings_restore(fd[SECTION_BINDINGS]))) {
        fprintf(stderr, "scene descriptor table refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_events_restore(fd[SECTION_EVENTS]))) {
        fprintf(stderr, "scene event table refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_handles_restore(fd[SECTION_HANDLES]))) {
        fprintf(stderr, "scene thread handle table refused: %d\n", rc); return 1;
    }
    if ((rc = dm_checkpoint_scene_images(fd[SECTION_IMAGES], 1))) {
        fprintf(stderr, "scene image storage refused: %d\n", rc); return 1;
    }
    if (mode != CHECKPOINT_PROBE_RETURN) {
        rc = dm_checkpoint_mounts(fd[SECTION_MOUNTS], section_dir, 1);
        if (rc) { LOG("[snapshot] field mounts refused (%d)", rc); return 1; }
    }
    if ((rc = survey_validate_threads(fd[SECTION_THREADS], selected, mode == CHECKPOINT_PROBE_RETURN ? NULL : all))) {
        LOG("[snapshot] scene thread continuations refused: %d", rc); return 1;
    }
    if (mode != CHECKPOINT_PROBE_RETURN) {
        rc = dm_checkpoint_audio_outputs(fd[SECTION_AUDIO], 1);
        if (rc) { LOG("[snapshot] audio outputs refused (%d)", rc); return 1; }
        rc = dm_checkpoint_graphics_timeline(fd[SECTION_TIMELINE], 1);
        if (rc) { LOG("[snapshot] graphics timeline refused (%d)", rc); return 1; }
        rc = dm_checkpoint_renderer(fd[SECTION_RENDERER], 1, &audio_worker);
        if (rc) { LOG("[snapshot] renderer state refused (%d)", rc); return 1; }
    }
    return 0;
}

int dm_checkpoint_replay_scene(const char *module_dir, const char *section_dir,
                               checkpoint_loader load, enum checkpoint_replay_mode mode,
                               int (*verify_return)(dm_checkpoint_wait_cpu *)) {
    int dir = open(section_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) { perror("open scene directory"); return 2; }
    int fd[SECTION_COUNT];
    unsigned opened = 0;
    unsigned required = mode == CHECKPOINT_PROBE_RETURN ? SECTION_FILES + 1 : SECTION_COUNT;
    for (; opened < required; ++opened) {
        fd[opened] = openat(dir, checkpoint_filenames[opened], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd[opened] < 0) { perror(checkpoint_filenames[opened]); break; }
    }
    close(dir);
    static survey_roster all;
    dm_checkpoint_wait_cpu selected;
    int rc = opened == required ? restore_resources(module_dir, section_dir, load, mode, fd, &all, &selected) : 2;
    while (opened) close(fd[--opened]);
    if (rc) return rc;
    if (mode != CHECKPOINT_PROBE_RETURN) return run_field(&all, mode == CHECKPOINT_PLAY);
    if ((rc = verify_return(&selected))) {
        LOG("[snapshot] cold field wait return refused: %d", rc);
        return 1;
    }
    LOG("[snapshot] cold PID %d validated field resources", getpid());
    puts("Cold host rebuilt field memory, modules, GPU pools, descriptor tables and event bindings.");
    return 0;
}
