cmake_minimum_required(VERSION 3.20)
project(vane LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# vane is a Linux-native tool. Reject other platforms early rather than
# failing later on missing fork()/waitpid(). See docs/platform-support.md.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "vane targets Linux only (Tier 1 platform). WSL2 satisfies this.")
endif()

# LLVM 19 pin: the fork-checkpoint mechanism relies on clang::Interpreter,
# which is an internal REPL-oriented API that shifts across LLVM versions.
# The validated spike environment (see spikes/README.md) is LLVM 19.
find_package(LLVM 19 REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
message(STATUS "Found Clang at ${Clang_DIR}")

# Version stamp from git describe (falls back to "dev").
set(VANE_VERSION "dev")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE VANE_GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(VANE_GIT_DESC)
    set(VANE_VERSION "${VANE_GIT_DESC}")
  endif()
endif()

# Shared compile options for anything linking libclang/libLLVM.
function(vane_apply_clang_targets tgt)
  target_include_directories(${tgt} PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
  )
  target_compile_definitions(${tgt} PRIVATE ${LLVM_DEFINITIONS})
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()

# ── vane main executable ────────────────────────────────────────────────────
add_executable(vane src/main.cpp)
vane_apply_clang_targets(vane)
target_compile_definitions(vane PRIVATE VANE_VERSION="${VANE_VERSION}")
target_link_libraries(vane PRIVATE clang-cpp LLVM)

# ── Spikes (validation experiments, opt-out via -DVANE_BUILD_SPIKES=OFF) ────
option(VANE_BUILD_SPIKES "Build the validation spikes" ON)
if(VANE_BUILD_SPIKES)
  add_subdirectory(spikes)
endif()
