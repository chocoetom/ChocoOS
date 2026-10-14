# Toolchain para ChocoOS (x86_64-elf freestanding).
# Uso (no MSYS2 UCRT64, apos setup):
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake
#   cmake --build build
#
# Tenta nesta ordem: x86_64-elf-g++ > clang++ (target elf) > g++ com flags freestanding.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Bare metal: never try to LINK a hosted test program (no crt0/libc here).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(X86_64_ELF_CXX x86_64-elf-g++)
find_program(X86_64_ELF_CC x86_64-elf-gcc)
find_program(CLANG_CXX clang++)
find_program(CLANG_CC clang)

if(X86_64_ELF_CXX AND X86_64_ELF_CC)
  set(CMAKE_C_COMPILER   ${X86_64_ELF_CC})
  set(CMAKE_CXX_COMPILER ${X86_64_ELF_CXX})
elseif(CLANG_CXX AND CLANG_CC)
  set(CMAKE_C_COMPILER   ${CLANG_CC})
  set(CMAKE_CXX_COMPILER ${CLANG_CXX})
  # C/C++ only: NASM must not receive these (it chokes on --target/-m*).
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:--target=x86_64-unknown-elf>"
                      "$<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64>")
  add_link_options(--target=x86_64-unknown-elf -fuse-ld=lld)
else()
  # Fallback: gcc nativo do MSYS2 com -ffreestanding (funciona p/ hello kernel)
  set(CMAKE_C_COMPILER   gcc)
  set(CMAKE_CXX_COMPILER g++)
endif()

find_program(NASM nasm REQUIRED)

set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
set(CMAKE_EXE_LINKER_FLAGS_INIT "")
