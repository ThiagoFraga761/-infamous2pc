#!/usr/bin/env bash
# CMAKE_C_COMPILER_LAUNCHER for the port: the lifted SPU sources (and the two
# runtime files built with them) go to clang, everything else to the compiler
# CMake chose. clang keeps the SPU helpers' vectors in registers where GCC
# spills them: inFAMOUS 2's lighting kernel runs ~1.3x faster. GCC-only
# --param options are dropped on the way.
# spu_cold.c's fast fix-up path must leave the upper halves of the vector
# registers alone, and clang puts a vzeroupper before the ret of a function
# that used ymm; its self-test falls back to the full thunk without this.
spu=0 cold=0
for a in "$@"; do
    case "$a" in
        */runtime/spu/spu_cold.c) spu=1 cold=1 ;;
        */port/spu/*.c|*/runtime/spu/spu_dispatch_mt.c) spu=1 ;;
    esac
done
if [ "$spu" = 1 ] && [ -z "${PORT_SPU_GCC:-}" ]; then
    shift
    args=()
    for a in "$@"; do
        case "$a" in --param=*) ;; *) args+=("$a") ;; esac
    done
    [ "$cold" = 1 ] && args=(-mllvm -x86-use-vzeroupper=0 "${args[@]}")
    exec clang "${args[@]}"
fi
exec "$@"
