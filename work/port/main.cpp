/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "settings.h"
#include "ui.h"
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include "guest_vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */
extern char        g_sys_fs_root[512];        /* the same, for the raw sys_fs syscalls */
void     cellGame_init_from_paramsfo(const char* sfo_path);   /* title id, name, version */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
int      cellGcm_fifo_busy(void);              /* why the last pass stopped short of put: 1 sem, 2 park, 4 behind */
void     cellGcm_fifo_kick_wait(unsigned ms);  /* idle wait, woken by a dry fence poll */
unsigned long long ps3_qpc_us(void);          /* monotonic microseconds */
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, Vulkan on Linux when built with it, and
 * the null backend's headless software path anywhere else -- the same
 * selection runtime/host/host_posix.c and the toolkit's boot harness make. All
 * four expose the same three entry points, so the frame clock below is
 * backend-agnostic. */
extern "C" int  spu_workload_replay_stuck(const char* path);   /* runtime/spu */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#elif defined(PS3RECOMP_RSX_VULKAN)
/* Linux, opt-in (-DPS3RECOMP_RSX_VULKAN=ON): rsx_vulkan_backend.h lists the
 * PS3RECOMP_VK_* switches -- PS3RECOMP_VK_WINDOW=1 to see the frames,
 * PS3RECOMP_VK_GUEST_PROGRAMS=1 to run the title's own shaders. */
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_vulkan_backend_pump_messages(void);
#  define rsx_backend_init    rsx_vulkan_backend_init
#  define rsx_backend_present rsx_vulkan_backend_present
#  define rsx_backend_pump    rsx_vulkan_backend_pump_messages
#  define RSX_BACKEND_NAME    "Vulkan"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif
extern "C" int  rsx_be_thread_presents_queued(void);   /* libs/video/rsx_be_thread.c */

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it.
 *
 * PS3_VBLANK_HZ sets the rate (default 60; the hardware's is 59.94). A title
 * that flips on vblank intervals runs at a fraction of it, so this is also the
 * knob for running one faster than it was shipped -- whether its game logic
 * keeps real time when it does is the title's business, not the clock's. The
 * period is kept in microseconds: the old 16 ms tick was 62.5 Hz. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

static DWORD WINAPI frame_clock(LPVOID)
{
    SetThreadDescription(GetCurrentThread(), L"rsx");
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");
    port_ui_backend_ready();   /* the start screen, if any: the boot waits on its "Play" */

    double hz = 60.0;
    { const char* e = getenv("PS3_VBLANK_HZ");
      if (e && *e) hz = atof(e);
      if (!(hz >= 1.0 && hz <= 1000.0)) {
          fprintf(stderr, "[rsx] PS3_VBLANK_HZ=%s out of range, using 60\n", e);
          hz = 60.0;
      } }
    const unsigned long long period_us = (unsigned long long)(1000000.0 / hz + 0.5);
    /* Between passes: while the FIFO walker is stopped short of `put` -- at a
     * semaphore or a park that an SPU will release by writing guest memory,
     * which wakes nobody -- poll again after PS3_DRAIN_NAP_US (default 200 us).
     * Otherwise wait up to 1 ms, woken early by a guest thread that polls a
     * fence the queue has run out of (cellGcm_fifo_kick_wait). This loop used
     * to Sleep(4) at every pass, and inFAMOUS 2's frame, which waits on SPU
     * labels several times, ran at two thirds of its rate. */
    long nap_us = 200;
    { const char* e = getenv("PS3_DRAIN_NAP_US"); if (e && *e) nap_us = atol(e); }
    if (nap_us < 0) nap_us = 0;
    if (nap_us > 999999) nap_us = 999999;
    if (hz != 60.0) fprintf(stderr, "[rsx] vblank at %.2f Hz\n", hz);
    unsigned long long next_tick = ps3_qpc_us();
    /* PS3_CLOCK_STATS=1: every 5 s, where this thread's time went -- waiting
     * at a semaphore (an SPU label), at a park, behind `put`, idle (nothing
     * queued: the PPU is the one behind), draining, presenting. */
    const int stats = getenv("PS3_CLOCK_STATS") != NULL;
    unsigned long long st_t0 = next_tick, st_wait[4] = {0, 0, 0, 0}, st_drain = 0, st_present = 0;
    unsigned long long last_boot_present = 0;
#define CLOCK_TIMED(acc, stmt) do { const unsigned long long _a = stats ? ps3_qpc_us() : 0; stmt; \
                                    if (stats) (acc) += ps3_qpc_us() - _a; } while (0)

    for (;;) {
        const int busy = rsx_ok ? cellGcm_fifo_busy() : 0;
        const unsigned long long w0 = stats ? ps3_qpc_us() : 0;
        if (busy) {
#ifdef _WIN32
            Sleep(nap_us >= 1000 ? (DWORD)(nap_us / 1000) : 0);
#else
            struct timespec ts = { 0, nap_us * 1000L }; nanosleep(&ts, NULL);
#endif
        } else cellGcm_fifo_kick_wait(1);
        unsigned long long now = ps3_qpc_us();
        if (stats) {
            st_wait[(busy & 1) ? 0 : (busy & 2) ? 1 : (busy & 4) ? 2 : 3] += now - w0;
            if (now - st_t0 >= 5000000ull) {
                const double T = (double)(now - st_t0) / 100.0;
                fprintf(stderr, "[clock] semaphore %.0f%%  park %.0f%%  behind-put %.0f%%  idle %.0f%%  "
                        "drain %.0f%%  present %.0f%%\n", st_wait[0] / T, st_wait[1] / T, st_wait[2] / T,
                        st_wait[3] / T, st_drain / T, st_present / T);
                st_t0 = now; st_wait[0] = st_wait[1] = st_wait[2] = st_wait[3] = st_drain = st_present = 0;
            }
        }

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                CLOCK_TIMED(st_present, present_guest_frame());
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) CLOCK_TIMED(st_drain, cellGcm_rsx_process_fifo());
            next_tick += period_us;
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                CLOCK_TIMED(st_present, present_guest_frame());
            }
            CLOCK_TIMED(st_drain, cellGcm_rsx_process_fifo());

            if (rsx_backend_pump() != 0) {
                /* The window was closed: that is the player quitting. Running
                 * on without it left a headless process behind. _Exit, not
                 * exit: the guest threads are still running, and destructors
                 * and atexit handlers would race them. */
                fprintf(stderr, "[rsx] window closed -- exiting\n");
                fflush(stderr);
                _Exit(0);
            }
            /* Present a flip the drain just reached, now: the drain stops at
             * the flip, so the batch is exactly the finished frame, and the
             * guest is waiting on this flip before it starts the next one.
             * Consuming the pending flag here matters -- this used to present
             * on the flip count changing and leave the flag set, so the check
             * above re-showed the same frame on the next pass with no draws:
             * every frame was presented twice. Before the first flip present
             * freely, so the window is not blank during boot. */
            if (cellGcm_take_flip_pending())
                CLOCK_TIMED(st_present, present_guest_frame());
            else if (cellGcm_flip_request_count() == 0 &&
                     now - last_boot_present >= (port_ui_start_screen() ? 16000ull : 4000ull) &&
                     rsx_be_thread_presents_queued() == 0) {
                /* Before the first flip: keep the window fresh, at the old 4 ms
                 * pace; about 60 a second under the start screen, which only
                 * needs the menu to follow the mouse. Never behind a present
                 * still queued: with VSync that wait would hold up the drain. */
                last_boot_present = now;
                CLOCK_TIMED(st_present, rsx_backend_present());
            }
        }
    }
#undef CLOCK_TIMED
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* The raw sys_fs syscalls translate on their own and default to the working
 * directory, which play.sh puts one level above the disc tree. The title opens
 * its save icon, ./USRDIR/ui/xmb/savedata/icon.png, that way: it missed, the
 * title handed cellSaveData an ICON0 with no buffer, and every save stopped
 * after PARAM.SFO. Point both layers at the same tree. */
static void sync_sys_fs_root(void)
{
    if ((size_t)snprintf(g_sys_fs_root, sizeof g_sys_fs_root, "%s", ppu_vfs_root)
            >= sizeof g_sys_fs_root)
        fprintf(stderr, "[boot] WARNING: VFS root longer than %zu characters; raw sys_fs "
                        "opens will miss\n", sizeof g_sys_fs_root - 1);
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
#endif
    /* 4 GB aligned and backed throughout -- demand-committed by the VEH on
     * Windows, lazily by the OS elsewhere (see guest_vm.h) -- so every 32-bit
     * guest offset is valid and no OOB guard is needed. */
    vm_base = guest_vm_map_flat((size_t)VM_SIZE);
    ppu_vm_size = 0;
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

extern "C" void port_native_passes_init(void);

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

    /* The player's settings (user/settings.ini) become the environment the
     * port reads, below anything already set there (settings.h). First, so
     * that every getenv after this sees them. Only with a window: a headless
     * run (the tests, the benchmarks) stays what its own environment says,
     * whatever the player chose. */
    const char* vk_window = getenv("PS3RECOMP_VK_WINDOW");
    if (vk_window && vk_window[0] && vk_window[0] != '0') { port_settings* ps = port_settings_get();
      const int had = port_settings_load(ps, port_settings_path());
      port_settings_from_env(ps);
      port_settings_export(ps);
      printf("[settings] %s %s: scale %d, voices %s, text %s, VSync %s\n",
             had ? "read" : "defaults, no", port_settings_path(), ps->scale, ps->voice, ps->text,
             ps->vsync ? "on" : "off");
      port_ui_init(argc, argv); }

    /* inFAMOUS 2: the audio sync the main thread waits on comes from its Wwise
     * jobs' own throw_event, so a job must not also get a synthetic "job done"
     * from the chain -- lifted (SPURS_JOB_LIFTED_RAN) or, for one with no lift,
     * on the interpreter (SPU_JOB_INTERP). An environment override still wins. */
#ifdef _WIN32
    if (!getenv("SPU_JOB_INTERP")) _putenv_s("SPU_JOB_INTERP", "1");
    if (!getenv("SPURS_JOB_LIFTED_RAN")) _putenv_s("SPURS_JOB_LIFTED_RAN", "1");
#else
    setenv("SPU_JOB_INTERP", "1", 0);
    setenv("SPURS_JOB_LIFTED_RAN", "1", 0);
    /* The Wwise audio thread notifies its chain's job guard in bursts; a
     * notify on an open guard must be lost, as on hardware, or the chain runs
     * laps over descriptors the title is still writing (libs/spurs/cellSpurs.c). */
    setenv("SPURS_GUARD_CAP", "1", 0);
    /* An idle SPU polls for work 200 us before it sleeps: the title hands
     * work over with its own atomics, which wake nobody, and a sleeping SPU
     * saw them only at its 1 ms timeout (libs/spurs/spurs_kernel.c). */
    setenv("SPURS_IDLE_SPIN_US", "200", 0);
    /* A Wwise job that ran and did not finish (the title had already refilled
     * its descriptor) gets no synthetic completion either: the WWS handlers
     * read one as a message and call through NULL. */
    setenv("SPURS_JC_NO_SYNTH", "1", 0);
#endif
    /* Its frames are built on the SPU: the FIFO parks at the end of what is
     * ready and the title patches the park with the rest. Wait at a park the
     * way the RSX does instead of following the write head. */
#ifdef _WIN32
    if (!getenv("GCM_PARK_PATIENCE_MS")) _putenv_s("GCM_PARK_PATIENCE_MS", "-1");
#else
    setenv("GCM_PARK_PATIENCE_MS", "-1", 0);
#endif
    /* The lighting composite waits on label 0x890, the frame number the SPU
     * lighting job writes when it is done: without the wait the RSX read
     * half-lit tiles. Bounded (GCM_SEMA_TIMEOUT_MS, 1 s) so a label nothing
     * writes cannot wedge the drain. */
#ifdef _WIN32
    if (!getenv("GCM_SEMA_ACQUIRE")) _putenv_s("GCM_SEMA_ACQUIRE", "1");
#else
    setenv("GCM_SEMA_ACQUIRE", "1", 0);
#endif
    /* The GPU does the work of two SPU jobs, the ambient occlusion and the
     * lighting (native_passes.c); each falls back to the SPU on any frame the
     * GPU cannot do. =0 turns one off, =2 compares the GPU's result with the
     * SPU's instead. */
#ifdef _WIN32
    if (!getenv("PS3RECOMP_NATIVE_SSAO")) _putenv_s("PS3RECOMP_NATIVE_SSAO", "1");
    if (!getenv("PS3RECOMP_NATIVE_LIGHTING")) _putenv_s("PS3RECOMP_NATIVE_LIGHTING", "1");
#else
    /* RSX local memory write tracking (runtime/memory/guest_wtrack.h): a
     * texture whose pages nobody wrote is not hashed again. =2 verifies it
     * (hashes anyway and logs "[wtrack] MISSED"), =0 turns it off. */
    setenv("PS3RECOMP_WTRACK", "1", 0);
    setenv("PS3RECOMP_NATIVE_SSAO", "1", 0);
    setenv("PS3RECOMP_NATIVE_LIGHTING", "1", 0);
#endif
    /* Before any SPU runs: it may gate lifted SPU functions. */
    port_native_passes_init();

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    /* PS3RECOMP_SPU_REPLAY=<file>: run a stuck SPURS job dumped by
     * SPU_DUMP_STUCK (work/play.sh sets it) with its lift and on the
     * interpreter, report both, and exit. Guest memory is empty here: a job
     * that DMAs more than its staged inputs reads zeros. */
    if (const char* replay = getenv("PS3RECOMP_SPU_REPLAY")) {
        return spu_workload_replay_stuck(replay) == 0 ? 0 : 1;
    }

    /* Paths first, before anything that may start a thread: setenv racing a
     * getenv on another thread is undefined in glibc. derive_vfs_root needs only
     * the path. */
    derive_vfs_root(argv[1]);
    sync_sys_fs_root();
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    /* Without it cellGame reports the placeholder title id BLES00000. */
    {
        char sfo[1100];
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

#ifndef _WIN32
    /* inFAMOUS 2 installs to /dev_hdd0 on its first boot: keep that beside the
     * disc tree, not inside it (the parent taken lexically -- "../" would
     * resolve through a symlinked disc dir). Firmware reads (/dev_flash) come
     * from RPCS3's install of the user's own PUP, Flatpak or native; the
     * runtime's own fallback is a Windows path. The environment wins for both. */
    if (!getenv("PS3_HDD0_ROOT")) {
        static char hdd0[1024];
        snprintf(hdd0, sizeof hdd0, "%s", ppu_vfs_root);
        char* slash = strrchr(hdd0, '/');
        if (slash && slash != hdd0) *slash = 0; else snprintf(hdd0, sizeof hdd0, ".");
        strncat(hdd0, "/hdd0", sizeof hdd0 - strlen(hdd0) - 1);
        if (mkdir(hdd0, 0755) != 0 && errno != EEXIST)
            fprintf(stderr, "[boot] WARNING: cannot create %s (%s)\n", hdd0, strerror(errno));
        setenv("PS3_HDD0_ROOT", hdd0, 1);
    }
    if (!getenv("PS3_DEV_FLASH") && getenv("HOME")) {
        static const char* rel[] = { ".var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash",
                                     ".config/rpcs3/dev_flash" };
        static char flash[1024];
        for (unsigned i = 0; i < sizeof rel / sizeof rel[0]; i++) {
            struct stat st;
            snprintf(flash, sizeof flash, "%s/%s", getenv("HOME"), rel[i]);
            if (stat(flash, &st) == 0 && S_ISDIR(st.st_mode)) {
                setenv("PS3_DEV_FLASH", flash, 1);
                break;
            }
        }
    }
    printf("[boot] /dev_hdd0 -> %s, /dev_flash -> %s\n",
           getenv("PS3_HDD0_ROOT"), getenv("PS3_DEV_FLASH") ? getenv("PS3_DEV_FLASH") : "(runtime default)");
#endif

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    port_ui_wait_start();

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
