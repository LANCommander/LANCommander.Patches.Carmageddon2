#!/bin/sh
# Builds the three c2patch delivery modes with MSYS2 mingw32 gcc, from one shared core:
#   build/dinput.dll                                       dinput.dll proxy
#   build/LANCommander.Interposer.Plugin.Carmageddon2.dll  LANCommander Interposer plugin
#   build/carma2patch.asi                                  ASI plugin (Ultimate ASI Loader etc.)
# CARMA2_VERSION (from CI tags) is embedded and logged; it defaults to "dev".
set -e
cd "$(dirname "$0")"
# Local MSYS2; in CI the msys2 MINGW32 shell already has i686 gcc on PATH.
command -v gcc >/dev/null 2>&1 || export PATH="/c/msys64/mingw32/bin:$PATH"
mkdir -p build build/obj

VERSION="${CARMA2_VERSION:-dev}"

# Interposer plugin defaults (Config.yml Plugins.Carmageddon2): the keys and values of
# dist/c2patch.ini as YAML, registered via InterposerRegisterPluginConfig on the first run.
awk '{ sub(/\r$/, "") }
     /^\[/ { gsub(/[][]/, ""); printf "L\"%s:\\n\"\n", $0; next }
     /^[A-Za-z]/ { i = index($0, "="); printf "L\"  %s: %s\\n\"\n", substr($0, 1, i - 1), substr($0, i + 1) }' \
    dist/c2patch.ini > build/c2patch_yaml.h

CFLAGS="-m32 -O2 -Wall -Wextra -Ibuild"
VERSDEF="-DC2PATCH_VERSION=\"$VERSION\""
LDFLAGS="-shared -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--kill-at"
LIBS="-lws2_32 -lwsock32 -ldxguid -luuid"

# shared core (the patch modules + the nettest harness, which stays inert unless C2_UNIT is set)
CORE="c2patch dinput_wrap render ai net netrate names gamepad iat nettest nettest_input nettest_probe"
OBJS=""
for f in $CORE; do
    gcc $CFLAGS "$VERSDEF" -c src/$f.c -o build/obj/$f.o
    OBJS="$OBJS build/obj/$f.o"
done

# one DLL per entry point
gcc $CFLAGS "$VERSDEF" $LDFLAGS -o build/dinput.dll \
    src/dinput_proxy.c $OBJS src/dinput.def $LIBS
gcc $CFLAGS "$VERSDEF" $LDFLAGS -o build/LANCommander.Interposer.Plugin.Carmageddon2.dll \
    src/interposer_plugin.c $OBJS src/interposer.def $LIBS
# an ASI has no exported entry point (the loader just LoadLibrary's it); without a .def mingw would
# otherwise auto-export every symbol, including the DirectX GUIDs pulled in by -ldxguid/-luuid
gcc $CFLAGS "$VERSDEF" $LDFLAGS -Wl,--exclude-all-symbols -o build/carma2patch.asi \
    src/asi.c $OBJS $LIBS

echo "built c2patch $VERSION:"
ls -1 build/dinput.dll build/LANCommander.Interposer.Plugin.Carmageddon2.dll build/carma2patch.asi
