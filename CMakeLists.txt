cmake_minimum_required(VERSION 3.21)
project(ChocoOS VERSION 0.7.0 LANGUAGES C CXX ASM_NASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Limine header (protocol repo; bootloader binary stays v12.9.3) ---
set(LIMINE_H_URL "https://raw.githubusercontent.com/limine-bootloader/limine-protocol/trunk/include/limine.h")
set(LIMINE_H "${CMAKE_BINARY_DIR}/_deps/limine.h")
if(NOT EXISTS "${LIMINE_H}")
  message(STATUS "Baixando limine.h (limine-protocol/trunk) ...")
  file(DOWNLOAD "${LIMINE_H_URL}" "${LIMINE_H}" SHOW_PROGRESS)
endif()

# --- Kernel freestanding x86_64 ---
set(KERNEL_ELF "${CMAKE_BINARY_DIR}/boot/chocoos.elf")

add_executable(chocoos.elf
  kernel/main.cpp
  kernel/boot.cpp
  kernel/boot.asm
  kernel/console.cpp
  kernel/serial.cpp
  kernel/mem/pmm.cpp
  kernel/mem/paging.cpp
  kernel/mem/heap.cpp
  kernel/mem/rt.cpp
  kernel/cpu/idt.cpp
  kernel/cpu/pic.cpp
  kernel/cpu/pit.cpp
  kernel/cpu/keyboard.cpp
  kernel/cpu/mouse.cpp
  kernel/cpu/isr.asm
  kernel/cpu/gdt.cpp
  kernel/cpu/gdt.asm
  kernel/cpu/sched.cpp
  kernel/cpu/switch.asm
  kernel/cpu/usermode.asm
  kernel/cpu/linux.asm
  kernel/gui/icon.cpp
  kernel/gui/wallpaper.cpp
  kernel/gui/wm.cpp
  kernel/shell.cpp
  kernel/proc/syscall.cpp
  kernel/proc/elf.cpp
  kernel/proc/fs.cpp
  kernel/proc/procfs.cpp
  kernel/proc/linux.cpp
  kernel/proc/proc.cpp
  kernel/proc/virtio.cpp
  kernel/proc/pci.cpp
  kernel/proc/virtio_net.cpp
  kernel/proc/vfs.cpp
  kernel/proc/fat32.cpp
  kernel/net/net.cpp
)

target_include_directories(chocoos.elf PRIVATE
  "${CMAKE_BINARY_DIR}/_deps"
  "${CMAKE_SOURCE_DIR}/kernel"
  "${CMAKE_SOURCE_DIR}/kernel/include"
)

# Generated asset headers are not visible to dep scanning on first build
# (they sit behind #ifdefs), so depend explicitly when they exist, and
# predefine their macros (the headers define content, not the switches).
if(EXISTS "${CMAKE_SOURCE_DIR}/kernel/gen/wallpaper.inc")
  set_property(SOURCE kernel/gui/wallpaper.cpp APPEND PROPERTY
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/kernel/gen/wallpaper.inc")
  target_compile_definitions(chocoos.elf PRIVATE ASSET_WALLPAPERS=1)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/kernel/gen/icons.inc")
  set_property(SOURCE kernel/gui/icon.cpp APPEND PROPERTY
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/kernel/gen/icons.inc")
  target_compile_definitions(chocoos.elf PRIVATE ASSET_ICONS=1)
endif()
# Baked UI font (scripts/build-font.py): proportional + anti-aliased chrome.
if(EXISTS "${CMAKE_SOURCE_DIR}/kernel/gen/uifont.inc")
  set_property(SOURCE kernel/console.cpp APPEND PROPERTY
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/kernel/gen/uifont.inc")
  target_compile_definitions(chocoos.elf PRIVATE ASSET_UIFONT=1)
endif()

# NOTE: C/C++ flags are scoped so NASM receives none (it chokes on -f*/--target).
target_compile_options(chocoos.elf PRIVATE
  "$<$<COMPILE_LANGUAGE:C,CXX>:-ffreestanding>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-stack-protector>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-stack-check>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-pic>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-pie>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-plt>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mabi=sysv>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mno-80387>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mno-mmx>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mno-3dnow>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mno-sse>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mno-sse2>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mno-red-zone>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-mcmodel=kernel>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-Wextra>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-Wpedantic>"
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

target_link_options(chocoos.elf PRIVATE
  -nostdlib
  -static
  -fno-pic -fno-pie
  -mno-red-zone
  -T "${CMAKE_SOURCE_DIR}/kernel/linker.ld"
)

set_target_properties(chocoos.elf PROPERTIES
  OUTPUT_NAME chocoos.elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/boot"
  SUFFIX ""
  LINK_DEPENDS "${CMAKE_SOURCE_DIR}/kernel/linker.ld"
)

add_custom_target(iso
  DEPENDS chocoos.elf
  COMMAND ${CMAKE_COMMAND} -E echo "Kernel OK: ${KERNEL_ELF}"
  COMMAND ${CMAKE_COMMAND} -E echo "Para gerar ISO rode (no MSYS2 UCRT64): bash scripts/make-iso.sh"
  COMMENT "Kernel compilado. Veja scripts/make-iso.sh para ISO."
)
