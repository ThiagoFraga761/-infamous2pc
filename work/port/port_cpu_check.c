/* The lifted SPU and PPU sources are built for PORT_SPU_MARCH and
 * PORT_PPU_MARCH (CMakeLists.txt). On a CPU without that ISA level the first
 * vector instruction is a SIGILL with no word of why -- possibly in a static
 * constructor, before main. This check is compiled for the baseline and runs
 * first (constructor priority 101), so the failure is a sentence instead. */
#include <stdio.h>
#include <stdlib.h>

/* Not under the MSVC ABI: clang-cl does not link the compiler-rt pieces
 * __builtin_cpu_supports needs. */
#if (defined(PORT_SPU_ISA_LEVEL) || defined(PORT_PPU_ISA_LEVEL)) && \
    (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
__attribute__((constructor(101))) static void port_cpu_check(void)
{
    __builtin_cpu_init();
#ifdef PORT_SPU_ISA_LEVEL
    if (!__builtin_cpu_supports(PORT_SPU_ISA_LEVEL)) {
        fprintf(stderr, "This build needs an %s CPU (the SPU code is compiled for it). "
                        "Rebuild with -DPORT_SPU_MARCH= (empty) for any x86-64.\n",
                PORT_SPU_ISA_LEVEL);
        exit(1);
    }
#endif
#ifdef PORT_PPU_ISA_LEVEL
    if (!__builtin_cpu_supports(PORT_PPU_ISA_LEVEL)) {
        fprintf(stderr, "This build needs an %s CPU (the PPU code is compiled for it). "
                        "Rebuild with -DPORT_PPU_MARCH= (empty) for any x86-64.\n",
                PORT_PPU_ISA_LEVEL);
        exit(1);
    }
#endif
}
#endif
