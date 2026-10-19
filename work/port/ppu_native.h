/* Native stand-ins for lifted PPU functions: the registry (port/ppu_native.cpp)
 * the lift's PPU_NATIVE_HOOK asks (tools/ppu_native_entries.py). */
#pragma once
#include <stdint.h>

struct ppu_context;

/* What the compare mode checks besides the registers the ABI keeps. */
#define PPU_NATIVE_RET_R3 1u
#define PPU_NATIVE_RET_F1 2u
#define PPU_NATIVE_RET_V2 4u

#define PPU_NATIVE_SPANS 4
typedef struct ppu_native_span { uint64_t ea, n; } ppu_native_span;

typedef struct ppu_native_desc {
    uint32_t addr;
    const char* name;
    /* nonzero: did the guest function's work */
    int (*fn)(struct ppu_context*);
    /* the lifted function, for the compare mode (may be null) */
    void (*lifted)(struct ppu_context*);
    /* the guest bytes a call with this entry state writes, at most
     * PPU_NATIVE_SPANS spans; null for a function that writes none */
    int (*writes)(const struct ppu_context*, ppu_native_span*);
    uint32_t ret;
} ppu_native_desc;

#ifdef __cplusplus
extern "C" {
#endif
int ppu_native_register(uint32_t addr, int (*fn)(struct ppu_context*));
int ppu_native_register_desc(const ppu_native_desc* d);
const ppu_native_desc* ppu_native_table(int* n);
int (*ppu_native_slot(uint32_t addr))(struct ppu_context*);
#ifdef __cplusplus
}
#endif
