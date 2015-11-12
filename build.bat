@echo off

set COMPILER_OPTIONS=-W4 -WX -wd4100 -Od -Zi -Oi -GR- -EHsc -nologo -I ..\..\extern\llvm_x64\include -I ..\..\extern\llvm\include
set BASE_DIR=%CD%

if not exist build mkdir -p build
pushd build

cl %COMPILER_OPTIONS% -Fmmain.map %BASE_DIR%\jtoy.cpp ..\..\extern\llvm\MinSizeRel\lib\LLVMAnalysis.lib /link -opt:ref

popd