cmake_minimum_required(VERSION 3.16)
project(NebulaTrim VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NEBULATRIM_TESTS "Build the export smoke test (requires ffmpeg)" OFF)

find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3)

# --- libmpv: pkg-config (Linux, MSYS2) or an unpacked SDK in MPV_DIR (include/ + lib/) ---
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(MPV QUIET IMPORTED_TARGET mpv)
endif()
if(MPV_FOUND)
  set(NT_MPV PkgConfig::MPV)
else()
  find_path(MPV_INCLUDE_DIR mpv/client.h HINTS ${MPV_DIR} PATH_SUFFIXES include)
  find_library(MPV_LIBRARY NAMES mpv libmpv mpv-2 libmpv-2 libmpv.dll.a HINTS ${MPV_DIR} PATH_SUFFIXES lib)
  if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
    message(FATAL_ERROR "libmpv not found. Install mpv (with libmpv) or pass -DMPV_DIR=<path to a libmpv SDK>")
  endif()
  add_library(nt_mpv UNKNOWN IMPORTED)
  set_target_properties(nt_mpv PROPERTIES
    IMPORTED_LOCATION "${MPV_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}")
  set(NT_MPV nt_mpv)
endif()

# --- Dear ImGui (vendored, only core + SDL3/OpenGL3 backends) ---
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC
  IMGUI_DISABLE_DEMO_WINDOWS
  IMGUI_DISABLE_DEBUG_TOOLS
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC SDL3::SDL3 ${CMAKE_DL_LIBS})

# --- App ---
add_executable(nebulatrim WIN32
  src/main.cpp
  src/app.cpp
  src/player.cpp
  src/media.cpp
  src/exporter.cpp
  src/process.cpp
  src/util.cpp
  src/gl.cpp
  src/ui.cpp)
target_compile_definitions(nebulatrim PRIVATE NEBULATRIM_VERSION="${PROJECT_VERSION}")
target_link_libraries(nebulatrim PRIVATE imgui SDL3::SDL3 ${NT_MPV} Threads::Threads)

if(MSVC)
  target_compile_options(nebulatrim PRIVATE /utf-8 /W3)
  target_compile_options(imgui PRIVATE /utf-8)
else()
  target_compile_options(nebulatrim PRIVATE -Wall -Wextra)
  # Smaller binaries: drop unused code and symbols in Release.
  foreach(t imgui nebulatrim)
    target_compile_options(${t} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-ffunction-sections -fdata-sections>)
  endforeach()
  target_link_options(nebulatrim PRIVATE $<$<CONFIG:Release,MinSizeRel>:-Wl,--gc-sections -s>)
endif()
if(MINGW)
  # Static C++ runtime: no libstdc++/libgcc DLLs to ship.
  target_link_options(nebulatrim PRIVATE -static-libgcc -static-libstdc++)
endif()

include(GNUInstallDirs)
install(TARGETS nebulatrim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
  install(FILES packaging/nebulatrim.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

# --- Smoke test: builds a project with several clips and exports it with ffmpeg ---
if(NEBULATRIM_TESTS)
  add_executable(export_test tests/export_test.cpp src/exporter.cpp src/media.cpp src/process.cpp src/util.cpp)
  target_include_directories(export_test PRIVATE src)
  target_link_libraries(export_test PRIVATE SDL3::SDL3 Threads::Threads)
endif()
