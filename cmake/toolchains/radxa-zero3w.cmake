set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(RADXA_SYSROOT "/home/user/sysroots/radxa" CACHE PATH "Radxa target sysroot")
set(CMAKE_SYSROOT "${RADXA_SYSROOT}")
# Match the board's GCC 12 C++ runtime, rather than the host's GCC 13 default.
set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc-12)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++-12)

# Ubuntu's cross compiler also searches its own newer target libc installation.
# Prefer the board's headers and libraries even over those compiler defaults.
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
    "${RADXA_SYSROOT}/usr/include/aarch64-linux-gnu;${RADXA_SYSROOT}/usr/include")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${CMAKE_C_STANDARD_INCLUDE_DIRECTORIES}")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L${RADXA_SYSROOT}/usr/lib/aarch64-linux-gnu -L${RADXA_SYSROOT}/lib/aarch64-linux-gnu -L${RADXA_SYSROOT}/usr/lib/gcc/aarch64-linux-gnu/12")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")

set(CMAKE_FIND_ROOT_PATH "${RADXA_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${RADXA_SYSROOT}")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${RADXA_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${RADXA_SYSROOT}/usr/lib/pkgconfig:${RADXA_SYSROOT}/usr/share/pkgconfig"
)
