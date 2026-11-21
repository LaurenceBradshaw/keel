# Copyright 2026 Laurence Bradshaw
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Pins clang-cl and lld-link to the MSVC toolset KEEL_MSVC_TOOLSET names (e.g. 14.42), so the build
# does not depend on a vcvars shell: without one (CMake Tools, a plain terminal) clang-cl takes the
# newest installed STL, whose version check may refuse it. Included before project().

if(NOT CMAKE_HOST_WIN32 OR NOT KEEL_MSVC_TOOLSET)
    return()
endif()

set(_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
execute_process(
    COMMAND "${_vswhere}" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
            -property installationPath
    OUTPUT_VARIABLE _vs OUTPUT_STRIP_TRAILING_WHITESPACE)
file(TO_CMAKE_PATH "${_vs}" _vs)
file(GLOB _tools LIST_DIRECTORIES true "${_vs}/VC/Tools/MSVC/${KEEL_MSVC_TOOLSET}.*")
if(NOT _tools)
    message(FATAL_ERROR "MSVC toolset ${KEEL_MSVC_TOOLSET} not found under ${_vs}/VC/Tools/MSVC")
endif()
list(SORT _tools COMPARE NATURAL ORDER DESCENDING)
list(GET _tools 0 _tools)
message(STATUS "MSVC toolset: ${_tools}")

# The flags beat INCLUDE and LIB, so a vcvars shell for another version cannot override them.
string(APPEND CMAKE_C_FLAGS_INIT " /vctoolsdir \"${_tools}\"")
string(APPEND CMAKE_CXX_FLAGS_INIT " /vctoolsdir \"${_tools}\"")
foreach(_kind EXE SHARED MODULE)
    string(APPEND CMAKE_${_kind}_LINKER_FLAGS_INIT " \"/vctoolsdir:${_tools}\"")
endforeach()
