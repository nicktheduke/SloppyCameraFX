cmake_minimum_required(VERSION 3.21)
project(SloppyCameraFX VERSION 1.0.3 LANGUAGES C CXX)

# SLOPPY CAMERA FX by nicktheduke. One SKSE plugin with switchable modules (combat\, weather\) on a shared core\.
#
# Self-contained build:
#   extern\CommonLibSSE-NG   CommonLibSSE-NG 6.7.1 (MIT), built from source here
#   extern\hde64             MinHook's instruction length decoder (BSD-2), used by CommonLib's patch safety check
#   deps\vcpkg               a vcpkg x64-windows-static-md-release prefix with spdlog, fmt, directxtk, directxmath,
#                            nlohmann-json, rapidcsv and xbyak. Point SCFX_VCPKG at your own prefix to use that instead.
set(SCFX_VCPKG "${CMAKE_CURRENT_SOURCE_DIR}/deps/vcpkg" CACHE PATH "vcpkg installed prefix (x64-windows-static-md-release)")
list(APPEND CMAKE_PREFIX_PATH "${SCFX_VCPKG}")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Keep this machine's folders out of the DLL: trim the source root from __FILE__ / std::source_location, and store
# only the PDB file name.
cmake_path(NATIVE_PATH CMAKE_CURRENT_SOURCE_DIR NORMALIZE SCFX_ROOT_NATIVE)
add_compile_options("/d1trimfile:${SCFX_ROOT_NATIVE}\\" "/d1trimfile:${CMAKE_CURRENT_SOURCE_DIR}/")
add_link_options("/PDBALTPATH:%_PDB%")

# CommonLibSSE-NG: SE + AE + VR, Xbyak trampolines, no tests, never the prebuilt download.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_PATCH_SAFETY ON CACHE BOOL "" FORCE)
set(COMMONLIB_PREBUILT OFF CACHE BOOL "" FORCE)
set(COMMONLIB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_HDE64 "${CMAKE_CURRENT_SOURCE_DIR}/extern/hde64" CACHE PATH "" FORCE)
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)

find_package(directxtk CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB SCFX_SOURCES CONFIGURE_DEPENDS core/*.cpp combat/*.cpp weather/*.cpp)
add_library(SloppyCameraFX SHARED ${SCFX_SOURCES})
target_precompile_headers(SloppyCameraFX PRIVATE core/PCH.h)
set_target_properties(SloppyCameraFX PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(SloppyCameraFX PRIVATE core combat weather include "${SCFX_VCPKG}/include/directxtk")
target_compile_definitions(SloppyCameraFX PRIVATE
	NDEBUG COMMONLIB_VERSION="6.7.1" SKYRIM NOMINMAX _WINDOWS _UNICODE UNICODE USING_XINPUT
	_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
target_compile_options(SloppyCameraFX PRIVATE /utf-8 /permissive- /Zc:preprocessor /EHsc /MP /O2 /Zi /W3 /external:W0)
target_link_options(SloppyCameraFX PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(SloppyCameraFX PRIVATE
	CommonLibSSE::CommonLibSSE Microsoft::DirectXTK nlohmann_json::nlohmann_json
	advapi32 ole32 version dbghelp d3dcompiler d3d11 dxgi dxguid bcrypt user32 shell32 xinput psapi)
