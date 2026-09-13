cmake_minimum_required(VERSION 3.15)
project(cemu C)

# --- toolchain / language ------------------------------------------------
# Strict C11 (matches old Makefile's -std=c11, not gnu11).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Mirrors the old Makefile CFLAGS exactly:
#   -O2 -Wall -Wextra -std=c11 -D__USE_MINGW_ANSI_STDIO=1 -Isrc -MMD -MP
# Header dependencies are tracked by CMake/Ninja automatically (depfile),
# so -MMD/-MP are not passed by hand.
add_compile_options(-O2 -Wall -Wextra -D__USE_MINGW_ANSI_STDIO=1)

# --- sources --------------------------------------------------------------
# GLOB_RECURSE is a superset of the old Makefile's four-level explicit
# wildcard (src/*.c ... src/*/*/*/*.c). CONFIGURE_DEPENDS makes CMake
# re-glob when sources are added/removed (no manual generator step).
file(GLOB_RECURSE SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# --- target ---------------------------------------------------------------
add_executable(cemu ${SRCS})
target_include_directories(cemu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(cemu PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")
if(WIN32)
  # host/sock_win.c (gdb stub) and host/display_win.c (GDI display backend)
  target_link_libraries(cemu PRIVATE ws2_32 gdi32 user32)
endif()

# --- cemugui: the RSP debug front-end (阶段 3.5 片 3) ------------------------
# A host-side tool (tools/front), not a machine part: it is the client of
# cemu's gdb stub over TCP. Reuses only src/host/sock_win.c for the socket.
if(WIN32)
  add_executable(cemugui
    "tools/front/front.c"
    "tools/front/rsp.c"
    "src/host/sock_win.c")
  target_include_directories(cemugui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_options(cemugui PRIVATE -Wall -Wextra -D__USE_MINGW_ANSI_STDIO=1)
  target_link_libraries(cemugui PRIVATE ws2_32 user32 gdi32 comctl32 dwmapi uxtheme)
endif()

# --- check target (dependency-edge lint, AGENTS.md 第七节) ----------------
add_custom_target(check
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/depcheck.sh"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Running dependency-edge check (tools/depcheck.sh)")
