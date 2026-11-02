# CMake toolchain: cross-compile opcan for 64-bit Windows with MinGW-w64 GCC (used by packaging/build-windows.sh,
# compilers from `nix develop .#windows`). The result links libgcc, libstdc++ and winpthreads statically, so
# opcan.exe imports only Windows system DLLs.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
set(CMAKE_RC_COMPILER ${_triple}-windres)
# search only the MinGW-w64 runtime prefix for libraries/packages (never the Linux host's, e.g. its glfw)
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libm.a OUTPUT_VARIABLE _libm OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_mingw_prefix "${_libm}/../.." ABSOLUTE)
set(CMAKE_FIND_ROOT_PATH ${_mingw_prefix})
set(CMAKE_PREFIX_PATH ${_mingw_prefix})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# fully static runtime: no libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll next to the exe
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
