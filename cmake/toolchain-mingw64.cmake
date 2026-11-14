# Cross-compile NebulaTrim for Windows (x86_64) from Linux with MinGW-w64
# (llvm-mingw or mingw-w64-gcc). Example:
#
#   MINGW_ROOT=/opt/llvm-mingw cmake -S . -B build-win \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#       -DSDL3_DIR=/path/SDL3-devel-mingw/x86_64-w64-mingw32/lib/cmake/SDL3 \
#       -DMPV_DIR=/path/mpv-dev-x86_64
#   cmake --build build-win -j
#
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_prefix x86_64-w64-mingw32)
set(_hints "$ENV{MINGW_ROOT}/bin")
find_program(CMAKE_C_COMPILER NAMES ${_prefix}-clang ${_prefix}-gcc HINTS ${_hints} REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_prefix}-clang++ ${_prefix}-g++ HINTS ${_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${_prefix}-windres HINTS ${_hints})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Never pick up the host's pkg-config files (they describe Linux libraries).
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
