#!/bin/bash
# Build SkyRecomp.exe for Windows (cross-compiled with llvm-mingw)
set -e
R=${SKYRECOMP:-$(cd "$(dirname "$0")/.." && pwd)}
TC=${LLVM_MINGW:-/tmp/toolchain/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}/bin
CC=$TC/x86_64-w64-mingw32-clang
CXX=$TC/x86_64-w64-mingw32-clang++
SDL=${SDL2_MINGW:-/tmp/sdl/SDL2-2.32.10/x86_64-w64-mingw32}
MDS=${MELONDS:-/tmp/melonDS}
cd $R/build-win
OPT=${OPT:--O2}
: > jobs.txt
for f in $R/gen/*.c; do
  o=obj/$(basename $f .c).o
  if [ ! -f $o ] || [ $f -nt $o ]; then echo "$CC $OPT -w $CFLAGS_EXTRA -I$R/runtime -c $f -o $o" >> jobs.txt; fi
done
[ -s jobs.txt ] && xargs -P2 -I{} sh -c '{}' < jobs.txt
$CC -O2 -w -I$R/runtime -c $R/runtime/native.c -o obj/native.o
$CXX -O2 -std=c++20 -w -I$MDS/src -I$MDS/build-win/src -I$R/runtime -c $R/runtime/hybrid.cpp -o obj/hybrid.o
$CXX -O2 -std=c++20 -w -I$MDS/src -I$MDS/build-win/src -c $R/frontend/platform.cpp -o obj/platform.o
$CXX -O2 -std=c++20 -w -I$MDS/src -I$MDS/build-win/src -I$R/runtime -I$SDL/include/SDL2 -Dmain=SDL_main ${DEV:+-DSKYRECOMP_DEV} -c $R/frontend/main.cpp -o obj/main.o
$CXX -O3 -std=c++20 -w -c $R/frontend/filters.cpp -o obj/filters.o
$CXX -o SkyRecomp.exe obj/*.o $MDS/build-win/src/libcore.a $(find $MDS/build-win/src -name "*.a" ! -name libcore.a) \
  -L$SDL/lib -lmingw32 -lSDL2main -lSDL2 -static-libstdc++ -static -lws2_32 -liphlpapi -lole32 -loleaut32 -lwinmm -lsetupapi -limm32 -lversion -lgdi32 -luser32 -lshell32 -luuid -ladvapi32 -lcfgmgr32 -ldinput8 -ldxguid -mwindows
rm -f SDL2.dll
echo built
