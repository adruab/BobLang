#!/bin/bash
# Build compiler using clang call

mkdir -p build
pushd build > /dev/null

CL="/usr/bin/clang++"
COMPILE_OPTIONS="-DPLATFORM_OSX=1 -g -Werror -O0 -std=c++0x -I/usr/local/opt/libffi/lib/libffi-3.0.13/include"
LD_FLAGS="-Wl,-no_compact_unwind"
$CL $COMPILE_OPTIONS $LD_FLAGS ../jtoy.cpp -o jtoy /usr/local/opt/libffi/lib/libffi.a

popd > /dev/null