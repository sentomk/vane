cmake_minimum_required(VERSION 3.20)
project(vane LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# vane is a Linux-native tool. Reject other platforms early with a clear
# message rather than failing later on missing fork(), waitpid(), etc.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "vane targets Linux only (Tier 1 platform). "
    "See docs/platform-support.md. WSL2 satisfies this requirement.")
endif()

# LLVM/Clang libraries. On Debian these live under /usr/lib/llvm-<v>/lib/cmake.
find_package(LLVM 17 REQUIRED CONFIG)
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

add_executable(vane
  src/main.cpp
)

target_include_directories(vane PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS}
)

target_compile_definitions(vane PRIVATE
  VANE_VERSION="${VANE_VERSION}"
  ${LLVM_DEFINITIONS}
)

# libTooling / Frontend surface. Kept minimal for the skeleton — expand as
# the fork-checkpoint executor lands.
target_link_libraries(vane PRIVATE
  clangTooling
  clangFrontend
  clangDriver
  clangSerialization
  clangParse
  clangSema
  clangAnalysis
  clangEdit
  clangAST
  clangLex
  clangBasic
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(vane PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
