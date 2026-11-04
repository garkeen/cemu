cmake_minimum_required(VERSION 3.20)
project(cesdk LANGUAGES NONE)

# The guest toolchain is clang/LLVM (arch.md 阶段 5: the compiler is one
# thing we do not write).  Point these at the local LLVM install; on the
# developer machine that is D:/LLVM/bin.
set(CESDK_CLANG "clang" CACHE FILEPATH "freestanding C compiler (clang)")
set(CESDK_LD "ld.lld" CACHE FILEPATH "ELF linker (ld.lld)")

set(CESDK_CFLAGS
    -ffreestanding -fno-builtin -fno-stack-protector -fno-pic
    -nostdlib -O2 -Wall -Wextra -Werror
    -I${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every image links the same runtime and the program's sources; only the
# target flags and the linker script differ.
function(cesdk_image name arch ldscript)
  set(out "${CMAKE_BINARY_DIR}/${name}.elf")
  set(objs)
  foreach(src ${ARGN})
    get_filename_component(stem "${src}" NAME_WE)
    set(obj "${CMAKE_BINARY_DIR}/${name}-${stem}.o")
    add_custom_command(
      OUTPUT "${obj}"
      COMMAND "${CESDK_CLANG}" ${CESDK_CFLAGS} ${arch}
              -c "${CMAKE_CURRENT_SOURCE_DIR}/${src}" -o "${obj}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
      COMMENT "CC ${name} ${src}"
      VERBATIM)
    list(APPEND objs "${obj}")
  endforeach()
  add_custom_command(
    OUTPUT "${out}"
    COMMAND "${CESDK_LD}" -T "${CMAKE_CURRENT_SOURCE_DIR}/${ldscript}"
            -o "${out}" ${objs}
    DEPENDS ${objs} "${CMAKE_CURRENT_SOURCE_DIR}/${ldscript}"
    COMMENT "LD ${name}.elf"
    VERBATIM)
  add_custom_target(${name} ALL DEPENDS "${out}")
endfunction()

set(CESDK_RV64 --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany)
set(CESDK_X86  --target=i386-unknown-elf -m32)

cesdk_image(hello-riscv64 "${CESDK_RV64}" linker/riscv64.ld
            crun/start_riscv64.S crun/runtime.c crun/klib.c hello/main.c)

cesdk_image(hello-x86 "${CESDK_X86}" linker/x86.ld
            crun/start_x86.S crun/runtime.c crun/klib.c hello/main.c)
