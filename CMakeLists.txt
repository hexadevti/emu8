# CMakeLists.txt — DESKTOP (SDL2) debug build of emu8. Does NOT touch the arduino-cli device
# build (that path never invokes CMake and never defines BOARD_DESKTOP). See desktop/README.md.
#
#   Windows (native .exe, gdb):   MSYS2 MinGW32 shell ->
#     cmake -G "MinGW Makefiles" -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
#   WSL/Linux (sanitizers rail):
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DEMU_SANITIZE=thread && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(emu8_desktop C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)   # debug tool -> default to a debuggable build
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3 -fno-omit-frame-pointer")

set(REPO ${CMAKE_CURRENT_SOURCE_DIR})

# Match the ESP32: `char` is effectively unsigned there, and the cores read memory bytes into `char`
# and do arithmetic on them (e.g. Apple II text glyph index `chr*7*8` for codes >= 0x80). x86/MinGW
# defaults `char` to SIGNED, which wraps those into out-of-bounds reads (garbage text). -funsigned-char
# makes the desktop behave like the device.
add_compile_options(-funsigned-char)

# --- ABI: 32-bit (ILP32) so long/pointer widths match the ESP32 (Xtensa LX). Toggle off only if you
#     accept the fidelity gap. SDL2 + the C++ runtime must then also be 32-bit. ---
option(EMU_M32 "Build 32-bit (-m32) to match ESP32 ILP32" ON)
if(EMU_M32)
  add_compile_options(-m32)
  add_link_options(-m32)
endif()

# --- Sanitizers (WSL/Linux clang/gcc rail): -DEMU_SANITIZE=address,undefined  or  thread ---
if(EMU_SANITIZE)
  add_compile_options(-fsanitize=${EMU_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${EMU_SANITIZE})
endif()

# --- Sources: the SAME core/shared code the device compiles, minus the hardware-only leaves
#     (replaced by src/desktop/*), minus PC-XT/fabgl (Phase 3) and the IIGS bring-up harnesses. ---
file(GLOB_RECURSE CORE_SRC CONFIGURE_DEPENDS
  ${REPO}/src/apple2/*.cpp ${REPO}/src/c64/*.cpp  ${REPO}/src/nes/*.cpp
  ${REPO}/src/atari/*.cpp  ${REPO}/src/iigs/*.cpp ${REPO}/src/msx/*.cpp
  ${REPO}/src/sms/*.cpp    ${REPO}/src/z80/*.cpp
  ${REPO}/src/shared/*.cpp)
list(REMOVE_ITEM CORE_SRC
  ${REPO}/src/shared/audio_amp.cpp      # -> src/desktop/audio_sdl.cpp
  ${REPO}/src/shared/speaker.cpp        # -> src/desktop/audio_sdl.cpp
  ${REPO}/src/shared/display_gfx.cpp    # -> src/desktop/display_sdl.cpp
  ${REPO}/src/shared/sd.cpp             # -> src/desktop/sd_host.cpp
  ${REPO}/src/shared/usbgamepad.cpp     # ESP32-S3 USB host driver (input via SDL instead)
  ${REPO}/src/shared/keyboardPs2.cpp    # CYD PS/2 path (input via SDL instead)
  ${REPO}/src/iigs/m0_bench.cpp         # bring-up harness (compiled only with -DIIGS_M0_BENCH)
  ${REPO}/src/iigs/cpu65816_test.cpp)   # bring-up harness

file(GLOB DESKTOP_SRC CONFIGURE_DEPENDS ${REPO}/src/desktop/*.cpp)
# PC-XT is now built for real on desktop (see PCXT_SRC below), so drop the empty stub that used to
# satisfy the linker. pcxt_stub.cpp defines pcxtSetup()/pcxtLoop()/... — keeping it would clash.
list(REMOVE_ITEM DESKTOP_SRC ${REPO}/src/desktop/pcxt_stub.cpp)

# PC-XT (Intel 8086): the machine core in src/pcxt/fabgl/ is a host-portable vendored fabgl subset
# (i8086 + PIC/PIT/i8042/RTC/BIOS + a state-only GraphicsAdapter); src/pcxt/*.cpp is the emu8 glue
# (allocation, run loop, CGA render push, disk mount). All ESP-isms resolve through the arduino_shim.
file(GLOB PCXT_SRC CONFIGURE_DEPENDS ${REPO}/src/pcxt/*.cpp ${REPO}/src/pcxt/fabgl/*.cpp)

# --- Dear ImGui (docking branch) — the desktop-only native UI shell (resizable window, menu bar,
#     settings dialog, debug panels). Vendored in third_party/imgui, DELIBERATELY OUTSIDE src/ so the
#     arduino-cli device build (which compiles src/** recursively) never tries to compile these
#     SDL-dependent files. Only this CMake desktop build references them. ---
set(IMGUI_DIR ${REPO}/third_party/imgui)
set(IMGUI_SRC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp)

# tiny386 (Intel i386 PC emulator): vendored C99 core (pc/i386/vga/ide/...) + the C++ glue
# (tiny386.cpp) + embedded BIOS arrays (tiny386_roms.c). Compiled into the desktop build so the
# core can be debugged with gdb; the device build compiles the same files via arduino-cli under src/.
file(GLOB TINY386_SRC CONFIGURE_DEPENDS ${REPO}/src/tiny386/*.c ${REPO}/src/tiny386/*.cpp)

# Compile the Arduino sketch as a C++ translation unit (copied so the compiler accepts the name).
configure_file(${REPO}/emu8.ino ${CMAKE_BINARY_DIR}/emu8_ino.cpp COPYONLY)

add_executable(emu8
  ${CMAKE_BINARY_DIR}/emu8_ino.cpp
  ${REPO}/globals.cpp
  ${CORE_SRC}
  ${DESKTOP_SRC}
  ${IMGUI_SRC}
  ${TINY386_SRC}
  ${PCXT_SRC})

# ImGui headers (imgui.h) + its SDL backends. PRIVATE: only the desktop target uses them.
target_include_directories(emu8 PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# arduino_shim MUST come first so #include <Arduino.h>/SD.h/EEPROM.h/SPI.h/FS.h/BLEDevice.h and
# "driver/i2s.h" etc. resolve to the shim instead of the real (absent) libraries.
target_include_directories(emu8 BEFORE PRIVATE
  ${REPO}/src/desktop/arduino_shim
  ${REPO})
# tiny386 vendored sources include their own headers by bare name ("pc.h", ...) -> put their dir on
# the include path. AFTER the shim/repo dirs so e.g. emu.h still resolves to the project's, not here.
target_include_directories(emu8 PRIVATE ${REPO}/src/tiny386)

target_compile_definitions(emu8 PRIVATE BOARD_DESKTOP)
# The codebase is full of Arduino-isms; keep it building (warnings on, not fatal).
target_compile_options(emu8 PRIVATE -Wno-write-strings -Wno-narrowing)

# --- SDL2 + threads. We use SDL_MAIN_HANDLED (main.cpp), so SDL2main is NOT needed (avoids the
#     MinGW SDL2main/SDL2 link-order trap). ---
find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(emu8 PRIVATE Threads::Threads)
if(TARGET SDL2::SDL2)
  target_link_libraries(emu8 PRIVATE SDL2::SDL2)
else()
  target_include_directories(emu8 PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(emu8 PRIVATE ${SDL2_LIBRARIES})
endif()

# Copy the MinGW + SDL2 runtime DLLs next to emu8.exe so it runs standalone (double-click / F5)
# WITHOUT needing C:\msys64\mingw32\bin on PATH. (libgcc may be _dw2 or _sjlj depending on the build.)
if(WIN32)
  get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(dll SDL2.dll libstdc++-6.dll libwinpthread-1.dll
              libgcc_s_dw2-1.dll libgcc_s_sjlj-1.dll libgcc_s_seh-1.dll)
    if(EXISTS "${MINGW_BIN}/${dll}")
      add_custom_command(TARGET emu8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:emu8>"
        VERBATIM)
    endif()
  endforeach()
endif()
