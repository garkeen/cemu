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
  target_link_libraries(cemugui PRIVATE ws2_32 user32 gdi32 comctl32)
endif()

# --- check target (dependency-edge lint, AGENTS.md 第七节) ----------------
# The checks are bash scripts. On Windows a bare `bash` on PATH is WSL's, which
# cannot run a D:/ path and fails under cmd/PowerShell (D23), so resolve the
# shell once at configure time — Git's bash first — and run it as a *login*
# shell: started non-interactively it inherits the caller's PATH, which has no
# /usr/bin, and then dirname/grep/sed are all missing (the checks would pass
# vacuously). Every entry point below therefore runs from cmd, PowerShell and a
# POSIX shell alike.
if(WIN32)
  find_program(CEMU_BASH NAMES bash.exe
    PATHS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramFiles}/Git/usr/bin"
          "$ENV{ProgramW6432}/Git/bin" "$ENV{LOCALAPPDATA}/Programs/Git/bin"
          "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin"
    NO_DEFAULT_PATH)
endif()
if(NOT CEMU_BASH)
  find_program(CEMU_BASH NAMES bash)
endif()
if(NOT CEMU_BASH)
  message(WARNING "no bash found: the check targets cannot run (see D23)")
else()
  message(STATUS "shell for check targets: ${CEMU_BASH}")
endif()

add_custom_target(check
  COMMAND "${CEMU_BASH}" -l "${CMAKE_CURRENT_SOURCE_DIR}/tools/depcheck.sh"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Running dependency-edge check (tools/depcheck.sh)")

# The machine suites: the same scripts the developer runs by hand, behind a
# target so the entry point does not depend on which shell the caller is in.
foreach(suite x86 riscv64)
  add_custom_target(check-${suite}
    COMMAND "${CEMU_BASH}" -l "${CMAKE_CURRENT_SOURCE_DIR}/test/${suite}/run.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running the ${suite} suite (test/${suite}/run.sh)")
endforeach()
