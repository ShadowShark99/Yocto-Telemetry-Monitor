# Cross-compile for 64-bit Arm Linux with Debian/Ubuntu's packaged toolchain
# (apt install g++-aarch64-linux-gnu). Used by CI as a quick cross-build check
# outside Yocto:
#
#   cmake -S app -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=$PWD/app/cmake/toolchains/aarch64-linux-gnu.cmake
#
# Inside BitBake none of this is needed: the cmake class generates a
# toolchain file for the target machine automatically.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Look for libraries and headers only in the target root, programs only on the host.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
