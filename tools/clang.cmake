# cemu toolchain: LLVM/clang on the mingw-w64 sysroot (2026-09-13 toolchain
# switch, user decision — mingw64 remains the headers/runtime, no longer the
# compiler). Configure with:
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=tools/clang.cmake
set(CMAKE_C_COMPILER "D:/LLVM/bin/clang.exe")
# The driver's default Windows target is MSVC (needs a Windows SDK we do not
# carry); the mingw-w64 target uses the D:/mingw64 headers and CRT that the
# tree already depends on. clang auto-locates that sysroot.
set(CMAKE_C_FLAGS_INIT "--target=x86_64-w64-mingw32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--target=x86_64-w64-mingw32")
