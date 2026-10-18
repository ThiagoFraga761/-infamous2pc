#!/usr/bin/env bash
# CMAKE_CXX_COMPILER_LAUNCHER for the port: the lifted PPU sources go to
# clang++, everything else to the compiler CMake chose. With the context
# pointer __restrict and the guest stores free of compiler barriers
# (ppu_recomp.h), clang keeps guest registers in host registers across guest
# loads and stores and the stores' cold out-of-line calls; GCC reloads them
# from the context after every one. PORT_PPU_GCC=1 keeps GCC.
ppu=0
for a in "$@"; do
    case "$a" in
        */src/recomp*/ppu_recomp_*.cpp) ppu=1 ;;
        # Defines the TLS slot the lifted code reads: it must come from the
        # same compiler (MinGW GCC's TLS is emulated, clang's is native).
        */runtime/ppu/ppu_trampoline_tls.cpp) ppu=1 ;;
    esac
done
if [ "$ppu" = 1 ] && [ -z "${PORT_PPU_GCC:-}" ]; then
    # A MinGW-w64 cross build (tools/mingw64.cmake): the same clang, aimed at
    # the MinGW target, so the objects link with the rest of the GCC build.
    # GCC is not an option there anyway: it needs over 6 GB per lifted unit.
    target=()
    case "$1" in *-w64-mingw32-g++) target=(--target="$(basename "$1" | sed 's/-g++$//')") ;; esac
    shift
    args=()
    for a in "$@"; do
        case "$a" in --param=*) ;; *) args+=("$a") ;; esac
    done
    # Whether a call can reach the context depends on whether the pointer was
    # handed to a call before it (noalias + "captured before"); LLVM gives up
    # past 32 blocks, and every inline store is several.
    exec clang++ "${target[@]}" -mllvm -dom-tree-reachability-max-bbs-to-explore=1000 "${args[@]}"
fi
exec "$@"
