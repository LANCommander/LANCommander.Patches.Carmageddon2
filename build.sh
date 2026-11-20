#!/bin/sh
# Builds build/dinput.dll (the c2patch proxy) with MSYS2 mingw32 gcc.
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
mkdir -p build
# Interposer plugin defaults (Config.yml Plugins.c2patch): the keys and values of dist/c2patch.ini as YAML
awk '{ sub(/\r$/, "") }
     /^\[/ { gsub(/[][]/, ""); printf "L\"%s:\\n\"\n", $0; next }
     /^[A-Za-z]/ { i = index($0, "="); printf "L\"  %s: %s\\n\"\n", substr($0, 1, i - 1), substr($0, i + 1) }' \
    dist/c2patch.ini > build/c2patch_yaml.h
gcc -m32 -O2 -Wall -Wextra -shared -static-libgcc -Ibuild -o build/dinput.dll \
    src/dinput_proxy.c src/c2patch.c src/render.c src/ai.c src/net.c src/netrate.c src/names.c src/gamepad.c src/nettest.c src/nettest_input.c src/nettest_probe.c src/iat.c src/dinput.def \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at -lws2_32 -lwsock32 -ldxguid -luuid
# the same DLL, named for .interposer\Plugins (a plugin called dinput.dll could be bound as the game's import)
cp build/dinput.dll build/c2patch.dll
objdump -p build/dinput.dll | grep -A4 "\[Ordinal/Name Pointer\]"
objdump -p build/dinput.dll | grep "DLL Name"
