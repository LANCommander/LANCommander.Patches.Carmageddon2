#!/bin/sh
# Builds build/dinput.dll (the c2patch proxy) with MSYS2 mingw32 gcc.
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
mkdir -p build
gcc -m32 -O2 -Wall -Wextra -shared -static-libgcc -o build/dinput.dll \
    src/dinput_proxy.c src/c2patch.c src/render.c src/ai.c src/net.c src/dinput.def \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at
objdump -p build/dinput.dll | grep -A4 "\[Ordinal/Name Pointer\]"
objdump -p build/dinput.dll | grep "DLL Name"
