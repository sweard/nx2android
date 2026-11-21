// Link as --wrap=main against build.py --manual-checkpoint objects. The SDK
// and all other modules come from the actual title's installed data pack.
#include "dm.h"
#include "checkpoint_memory.h"
#include "checkpoint_modules.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);
void svc_wait_checkpoint(uintptr_t heap, uintptr_t stack, const char *mods, int fd, int restore);
void svc_image_checkpoint(uintptr_t heap, int fd, int bindings, int restore);
void svc_gpu_checkpoint(uintptr_t heap, int memfd, int poolfd, int restore);
void sdk_worker_checkpoint(uintptr_t heap, uintptr_t stack, int restore);
void svc_alias_range(uint8_t **base, size_t *size, uint64_t *mapped);
void svc_guest_range(uint8_t **base, size_t *size);

static uint64_t svc(int num, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t regs[32] = {a, b, c};
    svc_dispatch(num, regs, 0);
    if (regs[0]) die("SVC %#x returned %#llx", num, regs[0]);
    return regs[1];
}

int __wrap_main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "replay-image")) {
        int in = open(argv[2], O_RDONLY), out = open(argv[3], O_CREAT | O_EXCL | O_RDWR, 0600);
        int svc_replay_scene_image(int, int);
        return in < 0 || out < 0 ? 2 : svc_replay_scene_image(in, out);
    }
    if (argc != 9) return 2;
    int save = !strcmp(argv[1], "save"), restore = !strcmp(argv[1], "restore");
    int collide = !strcmp(argv[1], "collide");
    if (!save && !restore && !collide) return 2;
    const char *mods[] = DM_GAME_MODULES;
    int mfd = open(argv[4], save ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY, 0600);
    if (mfd < 0) die("module section file: %s", strerror(errno));
    int rc = dm_checkpoint_modules_begin(argv[2], mods, DM_GAME_NMOD, save ? -1 : mfd);
    if (rc) die("cannot reserve module layout: %s", strerror(-rc));
    load_modules(argv[2], mods, DM_GAME_NMOD);
    if ((rc = dm_checkpoint_modules_baseline())) die("cannot track modules: %s", strerror(-rc));
    void (*set_argv)(char **) = guest_sym("_ZN2nn2os11SetHostArgvEPPc");
    char **(*get_argv)(void) = guest_sym("_ZN2nn2os11GetHostArgvEv");
    void (*set_argc)(int) = guest_sym("_ZN2nn2os11SetHostArgcEi");
    int (*get_argc)(void) = guest_sym("_ZN2nn2os11GetHostArgcEv");
    if (!set_argv || !get_argv || !set_argc || !get_argc) return 1;
    int pfd = open(argv[5], save ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY, 0600);
    if (pfd < 0) die("GPU section file: %s", strerror(errno));
    int fd = open(argv[3], save ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY, 0600);
    int flags = save ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY;
    int events = open(argv[6], flags, 0600), images = open(argv[7], flags, 0600);
    int bindings = open(argv[8], flags, 0600);
    if (events < 0 || images < 0 || bindings < 0) die("wait/image section files");
    if (fd < 0) die("section file: %s", strerror(errno));
    // Create a real collision without overwriting any process mapping.
    if (collide) {
        uint64_t base;
        if (pread(fd, &base, sizeof base, 56) != sizeof base) return 2;
        char *p = mmap((void *)(uintptr_t)base, getpagesize(), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != (void *)(uintptr_t)base) return 2;
        *p = 91;
        int rc = dm_checkpoint_memory_restore(fd);
        if (rc != -EADDRINUSE || *p != 91) return 1;
        puts("Address collision rejected; sentinel intact.");
        return 0;
    }
    if (save && dm_checkpoint_memory_begin()) die("cannot reserve checkpoint memory");
    if (restore) {
        int rc = dm_checkpoint_memory_restore(fd);
        if (rc) {
            uint8_t *base; size_t size;
            svc_guest_range(&base, &size);
            fprintf(stderr, "restore refused: %s, published=%d\n", strerror(-rc), base != NULL);
            return base ? 3 : 1;
        }
    }
    uint64_t heap = svc(0x29, 0, 4, 0), stack = svc(0x29, 0, 14, 0);
    uint8_t *alias; size_t cap; uint64_t mapped;
    svc_alias_range(&alias, &cap, &mapped);
    enum { page = 4096, heap_size = 18 << 20 };
    char *str = (char *)alias + 63 * page;  // crosses a bitmap word
    const char msg[] = "checkpoint in original Horizon memory";
    if (save) {
        svc(0x01, 0, heap_size, 0);
        svc(0x2c, (uintptr_t)str, 6 * page, 0);
        svc(0x2d, (uintptr_t)str + page * 2, page * 2, 0);
        svc(0x04, stack, heap, page);
        memcpy(str, msg, sizeof msg);
        *(uint64_t *)(str + page * 5) = stack;
        *(char **)stack = str;
        *(uint64_t *)(heap + page) = stack;
        *(uintptr_t *)(heap + page + 8) = (uintptr_t)get_argc;
        set_argv((char **)stack);
        set_argc(37);
        svc(0x04, stack + 0x10000, heap + 0x10000, 0x10000);
        sdk_worker_checkpoint(heap, stack, 0);
        svc(0x04, stack + 0x30000, heap + 0xb0000, 0x10000);
        svc_wait_checkpoint(heap, stack, argv[2], events, 0);
        extern void vk_init(void);
        vk_init();
        svc_image_checkpoint(heap, images, bindings, 0);
        if (dm_checkpoint_modules_save(mfd) || fsync(mfd)) die("module save failed");
        svc_gpu_checkpoint(heap, fd, pfd, 0);
        if (fsync(fd) || fsync(pfd)) die("section sync failed");
    } else {
        if (mapped != 4 * page || *(uint64_t *)(heap + page) != stack ||
            *(uint64_t *)(str + page * 5) != stack || *(char **)stack != str) return 1;
        // Exercise the original SDK with the pointers from the prior process.
        if (get_argc() == 37 || get_argv() == (char **)stack) return 1;
        int mr = dm_checkpoint_modules_restore(mfd);
        if (mr) {
            fprintf(stderr, "module restore refused: %s, sdk_untouched=%d\n", strerror(-mr), get_argc() != 37);
            return 1;
        }
        uintptr_t saved_fn = *(uintptr_t *)(heap + page + 8);
        if (saved_fn != (uintptr_t)get_argc || ((int (*)(void))saved_fn)() != 37) return 1;
        if (get_argv() != (char **)stack || strcmp(get_argv()[0], msg)) return 1;
        svc_gpu_checkpoint(heap, fd, pfd, 1);
        sdk_worker_checkpoint(heap, stack, 1);
        svc_wait_checkpoint(heap, stack, argv[2], events, 1);
        svc_image_checkpoint(heap, images, bindings, 1);
        uint64_t info[6] = {0};
        svc(0x06, (uintptr_t)info, 0, stack);
        if (info[0] != stack || info[1] != page || (uint32_t)info[2] != 0x0b) return 1;
        svc(0x05, stack, heap, page);
        if (*(char **)heap != str) return 1;
        svc(0x2c, (uintptr_t)str + page * 2, page * 2, 0);
        svc_alias_range(&alias, &cap, &mapped);
        if (mapped != 6 * page || str[page * 2] != 0) return 1;
    }
    printf("%s pid=%d main=%s heap=%p stack=%p alias=%p\n", argv[1], getpid(),
           dm_main_build(), (void *)heap, (void *)stack, (void *)alias);
    close(events); close(images); close(bindings);
    close(pfd);
    close(mfd);
    close(fd);
    fflush(stdout);
    if (save) raise(SIGKILL);  // model an abrupt process death while the worker is parked
    return 0;
}
