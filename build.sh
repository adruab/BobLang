#!/bin/bash
# Build compiler using clang call

mkdir -p build
pushd build > /dev/null

CL="/usr/bin/clang++"
COMPILE_OPTIONS_LLVM=`/usr/local/opt/llvm/bin/llvm-config --cxxflags --ldflags --system-libs --libs core mcjit native bitwriter`
COMPILE_DISABLE_WARNINGS="-Wno-nested-anon-types -Wno-missing-field-initializers"
COMPILE_OPTIONS="-DPLATFORM_OSX=1 -g -Wall -Werror -O0 -std=c++0x" #-I/usr/local/opt/libffi/lib/libffi-3.0.13/include
LD_FLAGS="-Wl,-no_compact_unwind"
$CL $COMPILE_OPTIONS $COMPILE_OPTIONS_LLVM $COMPILE_DISABLE_WARNINGS $LD_FLAGS ../jtoy.cpp -o jtoy # /usr/local/opt/libffi/lib/libffi.a

popd > /dev/null