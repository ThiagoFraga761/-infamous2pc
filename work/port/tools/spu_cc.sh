#!/usr/bin/env bash
# CMAKE_C_COMPILER_LAUNCHER for the port: the lifted SPU sources (and the two
# runtime files built with them) go to clang, everything else to the compiler
# CMake chose. clang keeps the SPU helpers' vectors in registers where GCC
# spills them: inFAMOUS 2's lighting kernel runs ~1.3x faster. GCC-only
# --param options are dropped on the way.
spu=0
for a in "$@"; do
    case "$a" in
        */port/spu/*.c|*/runtime/spu/spu_dispatch_mt.c|*/runtime/spu/spu_cold.c) spu=1 ;;
    esac
done
if [ "$spu" = 1 ] && [ -z "${PORT_SPU_GCC:-}" ]; then
    shift
    args=()
    for a in "$@"; do
        case "$a" in --param=*) ;; *) args+=("$a") ;; esac
    done
    exec clang "${args[@]}"
fi
exec "$@"
