#!/bin/sh
# Builds civprobe as msimg32.dll (default proxy) and version.dll, plus the test
# harness, with LLVM-MinGW (i686).
set -e
cd "$(dirname "$0")"
CC=${CC:-i686-w64-mingw32-clang}
# No -s: Defender's ML heuristic quarantined the stripped probe DLL (Contebrew.A!ml, 2026-10-28).
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer -shared -static"
mkdir -p build
# mimalloc (third_party/mimalloc, v2.5.2) as one release object: no debug checks, no
# collection pass at exit (game code still frees after the probe detaches).
$CC -O2 -DNDEBUG -DMI_STAT=1 -DMI_SKIP_COLLECT_ON_EXIT=1 -DMI_DEFAULT_DISALLOW_ARENA_ALLOC=1 -Ithird_party/mimalloc/include -c third_party/mimalloc/src/static.c -o build/mimalloc.o
MILIBS="-Ithird_party/mimalloc/include -lpsapi -lbcrypt -ladvapi32"
# Version resource (Properties > Details) for both proxy DLLs.
i686-w64-mingw32-windres version.rc -O coff -o build/version_res.o
$CC $CFLAGS -o build/msimg32.dll probe.c alloctrack.c topdown.c luagc.c luaguard.c mialloc.c watch.c cmdstream.c audioguard.c modules.c crtfail.c lastrun.c hang.c coretick.c smallblock.c uiqueue.c mapgrid.c gpuwatch.c msgqueue.c luaheap.c d3dstats.c texhandles.c terrain.c intro.c exes.c build/mimalloc.o build/version_res.o proxy_msimg32.def $MILIBS
$CC $CFLAGS -DPROXY_VERSION -o build/version.dll probe.c alloctrack.c topdown.c luagc.c luaguard.c mialloc.c watch.c cmdstream.c audioguard.c modules.c crtfail.c lastrun.c hang.c coretick.c smallblock.c uiqueue.c mapgrid.c gpuwatch.c msgqueue.c luaheap.c d3dstats.c texhandles.c terrain.c intro.c exes.c build/mimalloc.o build/version_res.o proxy_version.def $MILIBS
$CC -O2 -Wall -municode -static -s -Wl,--large-address-aware     -o build/CivilizationV_probetest.exe harness.c -lversion -lmsimg32 -lgdi32
# Allocation-tracking tests (run with AllocCrt=ucrtbase.dll in civprobe.ini).
$CC -O2 -fomit-frame-pointer -Wall -static -Wl,--large-address-aware -o build/CivilizationV_alloctest.exe alloctest.c -lmsimg32 -lgdi32
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_allocstress.exe allocstress.c -lmsimg32 -lgdi32
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_peaktest.exe peaktest.c -lmsimg32 -lgdi32
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_restest.exe restest.c -lmsimg32 -lgdi32
# msgqueue guard test: the hook against a stand-in queue (argument 0 = guard off).
$CC -O2 -Wall -static -DMSGQUEUE_TEST -o build/msgqueuetest.exe msgqueuetest.c msgqueue.c exes.c
# uiqueue test: handler-table thunks and data-dirty merging against a stand-in exe image.
$CC -O2 -Wall -static -DUIQUEUE_TEST -o build/uiqueuetest.exe uiqueuetest.c uiqueue.c exes.c
# luaheap test: the Lua and SQLite heap wrappers against stand-in heaps, 4 threads (+ the real
# database DLL's allocator when the game is installed).
$CC -O2 -Wall -static -DLUAHEAP_TEST -o build/luaheaptest.exe luaheaptest.c luaheap.c exes.c build/mimalloc.o $MILIBS
# texhandles test: the game's texture handle pool code, grown to 16,384 and to 32,768 handles, in
# the installed DX11 exe mapped as an image (skipped when the game isn't installed).
$CC -O2 -Wall -static -o build/texhandlestest.exe texhandlestest.c texhandles.c terrain.c exes.c
# terrain test: TerrainBaseSlots' code changes and count stub in the installed DX11 exe mapped as
# an image (skipped when the game isn't installed).
$CC -O2 -Wall -static -o build/terraintest.exe terraintest.c terrain.c exes.c
# d3dstats test: counts known D3D11 calls through the hot-patched d3d11.dll (needs a D3D11 GPU).
$CC -O2 -Wall -static -DD3DSTATS_TEST -o build/d3dstatstest.exe d3dstatstest.c d3dstats.c exes.c -ld3d11
# luagc test: a stand-in database DLL that imports lua_pushcclosure from the game's lua51_Win32.dll.
llvm-dlltool -m i386 -d lua51_test.def -l build/liblua51_test.a
$CC -O2 -Wall -shared -static -s -o build/luagc_testdb.dll luagc_testdb.c build/liblua51_test.a
$CC -O2 -Wall -municode -static -s -Wl,--large-address-aware -o build/CivilizationV_luagctest.exe luagctest.c -lmsimg32 -lgdi32
# mimalloc redirect test: a CRT-less DLL that allocates only through msvcr90 imports, and an exe whose
# manifest binds msvcr90 like the game (run with MiMalloc=1).
llvm-dlltool -m i386 -d msvcr90_test.def -l build/libmsvcr90_test.a
$CC -O1 -ffreestanding -fno-builtin -nostdlib -shared -s -o build/mitest_dll.dll mitest_dll.c build/libmsvcr90_test.a -lkernel32 -Wl,--entry,_DllMainCRTStartup@12
llvm-dlltool -m i386 -d msvcr90_exe.def -l build/libmsvcr90_exe.a
llvm-dlltool -m i386 -d msvcp90_exe.def -l build/libmsvcp90_exe.a
i686-w64-mingw32-windres mitest.rc -O coff -o build/mitest_res.o
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_mitest.exe mitest.c build/mitest_res.o build/libmsvcr90_exe.a build/libmsvcp90_exe.a -lmsimg32 -lgdi32
# VA-map label test: holds msvcr90 blocks past the 60 s snapshot (MiMalloc=1, Telemetry=1).
llvm-dlltool -m i386 -d milabel.def -l build/libmilabel.a
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_milabel.exe milabel.c build/mitest_res.o build/libmilabel.a -lmsimg32 -lgdi32
# luaL_unref guard test (needs a copy of the game's lua51_Win32.dll in build/).
llvm-dlltool -m i386 -d lua51_guard.def -l build/liblua51_guard.a
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_guardtest.exe guardtest.c build/mitest_res.o build/liblua51_guard.a -lmsimg32 -lgdi32
# Watchpoint test (Watch=1, WatchGlobalExport=g_watched).
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_watchtest.exe watchtest.c -lmsimg32 -lgdi32
# Command stream slack test (CommandStreamTestRets = the two marked call sites).
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_slacktest.exe slacktest.c -lmsimg32 -lgdi32
# Command stream capacity test (drives CivProbe_StreamAlloc on a fake stream half).
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_streamtest.exe streamtest.c -lmsimg32 -lgdi32
# Spare-halves allocation test (writes civprobe.ini for a child run of itself, then restores it).
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_sparetest.exe sparetest.c -lmsimg32 -lgdi32
# Audio guard test: a fake mss32.dll and an exe that imports from it.
llvm-dlltool -m i386 -d fakemss.def -l build/libfakemss.a
$CC -O2 -Wall -shared -static -o build/mss32.dll fakemss_dll.c fakemss_exports.def
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_audiotest.exe audiotest.c build/libfakemss.a -lmsimg32 -lgdi32
# Stack scan test: crashes children of itself in the exe, in a run-time DLL and by overflow.
$CC -O2 -Wall -shared -static -o build/stacktest_dll.dll stacktest_dll.c
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_stacktest.exe stacktest.c -lmsimg32 -lgdi32
# CRT failure test: a CRT-less DLL importing the failure paths from msvcr90, and an exe whose
# manifest binds msvcr90 like the game; the exe crashes children of itself through each path.
llvm-dlltool -m i386 -d crttest_msvcr90.def -l build/libcrttest_msvcr90.a
$CC -O1 -ffreestanding -fno-builtin -fno-omit-frame-pointer -fno-optimize-sibling-calls -nostdlib -shared -o build/crttest_dll.dll crttest_dll.c build/libcrttest_msvcr90.a -lkernel32 -Wl,--entry,_DllMainCRTStartup@12
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_crttest.exe crttest.c build/mitest_res.o build/libmsvcr90_exe.a -lmsimg32 -lgdi32
# Previous-launch check test: a child that crashes unhandled, then normal launches after it.
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_lastruntest.exe lastruntest.c -lmsimg32 -lgdi32
# Freeze watchdog test: writes its own ini for a child that stops taking window messages.
$CC -O2 -Wall -static -Wl,--large-address-aware -o build/CivilizationV_hangtest.exe hangtest.c -lmsimg32 -lgdi32
echo "built: build/msimg32.dll build/version.dll and test exes"
