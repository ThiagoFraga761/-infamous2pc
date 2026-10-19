/* inFAMOUS 2 (BCUS98125): reverse-engineering trace hooks.
 *
 * Off unless asked for; with no variable set, re_trace_init returns at once.
 *
 *   PS3RECOMP_RE_TRACE=0x9543C,0x60B1EC,...
 *       Log the entry (r3-r8, f1) and the return value (r3) of these lifted
 *       functions. The hook goes into the address -> function map the
 *       indirect-call dispatcher uses, so it sees every call made through a
 *       pointer (vtables, script members, callbacks) and none made by a
 *       direct `bl`: pick virtual slots and script entry points, and watch
 *       memory for the rest. Up to RE_MAX_HOOKS addresses. A function that
 *       ends in a tail call returns before that call runs, so its "ret" line
 *       can show a stale r3.
 *   PS3RECOMP_RE_DUMP=0x40         also dump this many bytes at r3 on entry
 *   PS3RECOMP_RE_MAX=200           calls logged per hook (0 = no limit)
 *
 *   PS3RECOMP_RE_WATCH=0x9F7C24:4,0x9BFED0>0x334:4,...
 *       Poll guest memory every PS3RECOMP_RE_WATCH_MS (default 100) and log
 *       each change (up to 64 bytes a watch). An address is a path: a base,
 *       then any number of ">off" steps, each reading a pointer and adding
 *       off -- "0x9BFED0>0x334" is field 0x334 of the object the global at
 *       0x9BFED0 points to, followed wherever it moves. ("*A+off" is the
 *       same as "A>off".)
 *   PS3RECOMP_RE_TABLE=<array path>;<count path>;<stride>[|...]
 *       Dump a whole {pointer, count} array (stride bytes an element, up to
 *       4096 elements) each time its count changes, e.g.
 *       "0x9F7C20;0x9F7C24;8" or "0x9BFED0>0x14>0x44;0x9BFED0>0x14>0x48;32".
 *
 *   PS3RECOMP_RE_NAMES=work/runs/re/names.txt   label hooks with names
 *       (output of work/tools/re/who.py --names; local, untracked)
 *   PS3RECOMP_RE_LOG=path                       log there instead of stderr
 *
 * Lines start with "[re]" and carry a millisecond clock, so a run's log can be
 * matched against what was done in the game. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppu_context.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

extern uint8_t* vm_base;
typedef void (*re_fn)(ppu_context*);
typedef struct { uint64_t addr; re_fn func; const char* name; } re_func_entry;
/* Emitted by the lifter beside the lifted code (ppu_recomp.h, func_entry). */
extern const re_func_entry function_table[];
extern const uint64_t function_table_count;
void ppu_register_function(uint64_t addr, re_fn fn);

#define RE_MAX_HOOKS   32
#define RE_MAX_WATCHES 32
#define RE_MAX_TABLES  8

static FILE* g_log;
static unsigned g_dump, g_max = 200;

static uint64_t re_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}

static uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Guest addresses we may read: the EBOOT image, its .bss and the heap the
 * game allocates in user memory. Anything else is reported, not read. */
static int readable(uint32_t ea, uint32_t n)
{
    return vm_base && ea >= 0x10000u && ea + n > ea && ea + n <= 0x40000000u;
}

static void hexdump(const char* tag, uint32_t ea, uint32_t n)
{
    if (!readable(ea, n)) { fprintf(g_log, "[re]   %s 0x%08X: unreadable\n", tag, ea); return; }
    for (uint32_t o = 0; o < n; o += 16) {
        char line[80];
        int p = snprintf(line, sizeof line, "%08X:", ea + o);
        for (uint32_t i = o; i < n && i < o + 16; i += 4)
            p += snprintf(line + p, sizeof line - p, " %08X", rd32(ea + i));
        fprintf(g_log, "[re]   %s %s\n", tag, line);
    }
}

/* ---- names ------------------------------------------------------------- */
static struct { uint32_t addr; char name[64]; } *g_names;
static unsigned g_nnames;

static const char* name_of(uint32_t a)
{
    for (unsigned i = 0; i < g_nnames; i++)
        if (g_names[i].addr == a) return g_names[i].name;
    return "";
}

static void load_names(const char* path, const uint32_t* want, unsigned n)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(g_log, "[re] cannot open names file %s\n", path); return; }
    g_names = calloc(n ? n : 1, sizeof *g_names);
    char line[256];
    while (g_names && fgets(line, sizeof line, f)) {
        char* end;
        const uint32_t a = (uint32_t)strtoul(line, &end, 16);
        for (unsigned i = 0; i < n; i++) {
            if (want[i] != a || g_nnames >= n) continue;
            while (*end == ' ') end++;
            end[strcspn(end, "\r\n")] = 0;
            g_names[g_nnames].addr = a;
            snprintf(g_names[g_nnames].name, sizeof g_names[g_nnames].name, "%s", end);
            g_nnames++;
        }
    }
    fclose(f);
}

/* ---- function hooks ------------------------------------------------------ */
static struct {
    uint32_t addr;
    re_fn orig;
    unsigned calls;
} g_hooks[RE_MAX_HOOKS];
static unsigned g_nhooks;

static void hook_body(unsigned k, ppu_context* ctx)
{
    const unsigned n = __atomic_add_fetch(&g_hooks[k].calls, 1, __ATOMIC_RELAXED);
    const int log = !g_max || n <= g_max;
    const uint32_t r3 = (uint32_t)ctx->gpr[3];
    if (log) {
        fprintf(g_log, "[re] %llu t%u > %08X %s #%u r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X f1=%g\n",
                (unsigned long long)re_ms(), (unsigned)ctx->thread_id, g_hooks[k].addr,
                name_of(g_hooks[k].addr), n, r3, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8], ctx->fpr[1]);
        if (g_dump) hexdump("r3", r3, g_dump);
    }
    g_hooks[k].orig(ctx);
    if (log)
        fprintf(g_log, "[re] %llu t%u < %08X ret=%08X\n", (unsigned long long)re_ms(),
                (unsigned)ctx->thread_id, g_hooks[k].addr, (uint32_t)ctx->gpr[3]);
    if (g_max && n == g_max)
        fprintf(g_log, "[re] %08X: %u calls logged, the rest are counted only\n", g_hooks[k].addr, n);
}

/* One entry point per slot: the dispatcher hands a hook only the context. */
#define H(k) static void hook_##k(ppu_context* c) { hook_body(k, c); }
H(0) H(1) H(2) H(3) H(4) H(5) H(6) H(7) H(8) H(9) H(10) H(11) H(12) H(13) H(14) H(15)
H(16) H(17) H(18) H(19) H(20) H(21) H(22) H(23) H(24) H(25) H(26) H(27) H(28) H(29) H(30) H(31)
#undef H
static const re_fn g_entry[RE_MAX_HOOKS] = {
    hook_0, hook_1, hook_2, hook_3, hook_4, hook_5, hook_6, hook_7, hook_8, hook_9, hook_10,
    hook_11, hook_12, hook_13, hook_14, hook_15, hook_16, hook_17, hook_18, hook_19, hook_20,
    hook_21, hook_22, hook_23, hook_24, hook_25, hook_26, hook_27, hook_28, hook_29, hook_30,
    hook_31,
};

static re_fn lifted(uint32_t addr)
{
    for (uint64_t i = 0; i < function_table_count; i++)
        if ((uint32_t)function_table[i].addr == addr) return function_table[i].func;
    return NULL;
}

/* ---- memory watches ---------------------------------------------------- */
typedef struct { uint32_t base; uint32_t step[6]; unsigned nstep; } re_path;

static int resolve(const re_path* p, uint32_t* ea)
{
    uint32_t a = p->base;
    for (unsigned i = 0; i < p->nstep; i++) {
        if (!readable(a, 4)) return 0;
        a = rd32(a);
        if (!a) return 0;
        a += p->step[i];
    }
    *ea = a;
    return 1;
}

static const char* parse_path(const char* s, re_path* p)
{
    char* e;
    memset(p, 0, sizeof *p);
    int star = 0;
    while (*s == ' ') s++;
    if (*s == '*') { star = 1; s++; }
    p->base = (uint32_t)strtoul(s, &e, 16);
    s = e;
    if (star) {
        uint32_t off = 0;
        if (*s == '+') { off = (uint32_t)strtoul(s + 1, &e, 16); s = e; }
        p->step[p->nstep++] = off;
    }
    while (*s == '>' && p->nstep < 6) {
        p->step[p->nstep++] = (uint32_t)strtoul(s + 1, &e, 16);
        s = e;
    }
    return s;
}

static void path_str(const re_path* p, char* out, size_t n)
{
    int k = snprintf(out, n, "%08X", p->base);
    for (unsigned i = 0; i < p->nstep && k < (int)n; i++)
        k += snprintf(out + k, n - k, ">0x%X", p->step[i]);
}

static struct { re_path at; uint32_t len; uint8_t last[64]; int have; } g_watch[RE_MAX_WATCHES];
static unsigned g_nwatch, g_watch_ms = 100;

static struct { re_path arr, cnt; uint32_t stride, last; int have; } g_table[RE_MAX_TABLES];
static unsigned g_ntable;

static void watch_poll(void)
{
    for (unsigned i = 0; i < g_nwatch; i++) {
        uint32_t ea;
        if (!resolve(&g_watch[i].at, &ea) || !readable(ea, g_watch[i].len)) continue;
        uint8_t now[64];
        memcpy(now, vm_base + ea, g_watch[i].len);
        if (g_watch[i].have && !memcmp(now, g_watch[i].last, g_watch[i].len)) continue;
        char hex[200], ps[96];
        int p = 0;
        for (uint32_t k = 0; k < g_watch[i].len; k++) {
            p += snprintf(hex + p, sizeof hex - p, "%02X", now[k]);
            if ((k & 3) == 3 && k + 1 < g_watch[i].len) hex[p++] = ' ';
        }
        hex[p] = 0;
        path_str(&g_watch[i].at, ps, sizeof ps);
        fprintf(g_log, "[re] %llu watch %s (=%08X) = %s\n", (unsigned long long)re_ms(), ps, ea, hex);
        memcpy(g_watch[i].last, now, g_watch[i].len);
        g_watch[i].have = 1;
    }
    for (unsigned i = 0; i < g_ntable; i++) {
        uint32_t ca, aa;
        if (!resolve(&g_table[i].cnt, &ca) || !readable(ca, 4) || !resolve(&g_table[i].arr, &aa) ||
            !readable(aa, 4))
            continue;
        const uint32_t cnt = rd32(ca), arr = rd32(aa);
        if (g_table[i].have && cnt == g_table[i].last) continue;
        g_table[i].last = cnt;
        g_table[i].have = 1;
        char ps[96];
        path_str(&g_table[i].arr, ps, sizeof ps);
        fprintf(g_log, "[re] %llu table %s: %u elements at %08X\n", (unsigned long long)re_ms(), ps, cnt, arr);
        const uint32_t n = cnt < 4096 ? cnt : 4096, st = g_table[i].stride;
        if (!readable(arr, n * st)) continue;
        for (uint32_t k = 0; k < n; k++) {
            char line[160];
            int p = snprintf(line, sizeof line, "[re]   [%u]", k);
            for (uint32_t o = 0; o < st && o < 64 && p < (int)sizeof line - 10; o += 4)
                p += snprintf(line + p, sizeof line - p, " %08X", rd32(arr + k * st + o));
            fprintf(g_log, "%s\n", line);
        }
    }
    fflush(g_log);
}

#ifdef _WIN32
static DWORD WINAPI watch_thread(LPVOID u)
{
    (void)u;
    for (;;) { watch_poll(); Sleep(g_watch_ms); }
    return 0;
}
#else
static void* watch_thread(void* u)
{
    (void)u;
    for (;;) { watch_poll(); usleep(g_watch_ms * 1000u); }
    return NULL;
}
#endif

static void parse_watches(const char* s)
{
    while (s && *s && g_nwatch < RE_MAX_WATCHES) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        const unsigned i = g_nwatch++;
        memset(&g_watch[i], 0, sizeof g_watch[i]);
        s = parse_path(s, &g_watch[i].at);
        g_watch[i].len = 4;
        if (*s == ':') {
            char* e;
            g_watch[i].len = (uint32_t)strtoul(s + 1, &e, 0);
            s = e;
        }
        if (g_watch[i].len == 0 || g_watch[i].len > sizeof g_watch[i].last) g_watch[i].len = 4;
        while (*s && *s != ',') s++;
    }
}

static void parse_tables(const char* s)
{
    while (s && *s && g_ntable < RE_MAX_TABLES) {
        while (*s == '|' || *s == ' ') s++;
        if (!*s) break;
        const unsigned i = g_ntable;
        s = parse_path(s, &g_table[i].arr);
        if (*s != ';') break;
        s = parse_path(s + 1, &g_table[i].cnt);
        if (*s != ';') break;
        char* e;
        g_table[i].stride = (uint32_t)strtoul(s + 1, &e, 0);
        s = e;
        if (g_table[i].stride && g_table[i].stride <= 256) g_ntable++;
        while (*s && *s != '|') s++;
    }
}

/* Call once the lifted function table is registered (after
 * ppu_recomp_register), so these entries overwrite the lifted ones. */
void re_trace_init(void)
{
    const char* tr = getenv("PS3RECOMP_RE_TRACE");
    const char* wa = getenv("PS3RECOMP_RE_WATCH");
    const char* tb = getenv("PS3RECOMP_RE_TABLE");
    if (!tr && !wa && !tb) return;
    const char* lp = getenv("PS3RECOMP_RE_LOG");
    g_log = lp ? fopen(lp, "a") : NULL;
    if (!g_log) g_log = stderr;
    if (getenv("PS3RECOMP_RE_DUMP")) g_dump = (unsigned)strtoul(getenv("PS3RECOMP_RE_DUMP"), NULL, 0) & ~3u;
    if (g_dump > 0x400) g_dump = 0x400;
    if (getenv("PS3RECOMP_RE_MAX")) g_max = (unsigned)strtoul(getenv("PS3RECOMP_RE_MAX"), NULL, 0);
    if (getenv("PS3RECOMP_RE_WATCH_MS")) g_watch_ms = (unsigned)strtoul(getenv("PS3RECOMP_RE_WATCH_MS"), NULL, 0);
    if (g_watch_ms < 10) g_watch_ms = 10;

    uint32_t want[RE_MAX_HOOKS];
    unsigned nwant = 0;
    for (const char* s = tr; s && *s && nwant < RE_MAX_HOOKS;) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        char* e;
        want[nwant++] = (uint32_t)strtoul(s, &e, 16);
        s = e;
        while (*s && *s != ',') s++;
    }
    if (getenv("PS3RECOMP_RE_NAMES")) load_names(getenv("PS3RECOMP_RE_NAMES"), want, nwant);
    for (unsigned i = 0; i < nwant; i++) {
        const re_fn f = lifted(want[i]);
        if (!f) { fprintf(g_log, "[re] %08X is not a lifted function, skipped\n", want[i]); continue; }
        g_hooks[g_nhooks].addr = want[i];
        g_hooks[g_nhooks].orig = f;
        ppu_register_function(want[i], g_entry[g_nhooks]);
        fprintf(g_log, "[re] hooked %08X %s (indirect calls only)\n", want[i], name_of(want[i]));
        g_nhooks++;
    }

    parse_watches(wa);
    parse_tables(tb);
    for (unsigned i = 0; i < g_nwatch; i++) {
        char ps[96];
        path_str(&g_watch[i].at, ps, sizeof ps);
        fprintf(g_log, "[re] watch %s len %u\n", ps, g_watch[i].len);
    }
    for (unsigned i = 0; i < g_ntable; i++) {
        char ps[96];
        path_str(&g_table[i].arr, ps, sizeof ps);
        fprintf(g_log, "[re] table %s stride %u\n", ps, g_table[i].stride);
    }
    if (g_nwatch || g_ntable) {
#ifdef _WIN32
        CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
#else
        pthread_t t;
        pthread_create(&t, NULL, watch_thread, NULL);
        pthread_detach(t);
#endif
    }
    fflush(g_log);
}
