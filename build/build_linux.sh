#!/bin/bash
# Build the Linux test harness: melonDS core + recompiled game code + hybrid runtime
set -e
R=${SKYRECOMP:-$(cd "$(dirname "$0")/.." && pwd)}
MDS=${MELONDS:-/tmp/melonDS}
cd $R/build
OBJ=obj
for f in $R/gen/*.c; do
  o=$OBJ/$(basename $f .c).o
  if [ ! -f $o ] || [ $f -nt $o ]; then echo "cc $f"; echo "clang ${OPT:--O1} -w $CFLAGS_EXTRA -I$R/runtime -c $f -o $o"; fi
done > jobs.txt
grep '^clang' jobs.txt | xargs -P2 -I{} sh -c '{}'
clang -O2 -w -I$R/runtime -c $R/runtime/native.c -o $OBJ/native.o
clang++ -O2 -std=c++20 -w -I$MDS/src -I$MDS/build-core/src -I$R/runtime -c $R/runtime/hybrid.cpp -o $OBJ/hybrid.o
clang++ -O2 -std=c++20 -w -I$MDS/src -I$MDS/build-core/src -I$R/runtime -c $R/referee/harness.cpp -o $OBJ/harness.o
g++ -flto=auto -o $R/build/recomp_harness $OBJ/*.o $MDS/build-core/src/libcore.a $(find $MDS/build-core -name "*.a" ! -name libcore.a) -lpthread
echo built
