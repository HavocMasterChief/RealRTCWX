# CMake build for RealRTCW, aimed at MSVC (desktop first, then Xbox UWP).
# The original Makefile is untouched and still drives the MinGW/Linux/macOS
# builds; this file only exists so the engine can be built with Microsoft's
# toolchain, which the Xbox (UWP) target requires.

cmake_minimum_required(VERSION 3.21)
project(RealRTCW C CXX ASM_MASM)

if(NOT MSVC)
  message(FATAL_ERROR "This CMake build targets MSVC only. Use the Makefile for other compilers.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Only x64 is supported.")
endif()

option(RTCW_UWP "Build for Xbox (UWP Dev Mode) instead of desktop Windows" OFF)
set(SDL3_UWP_DIR "" CACHE PATH "SDL3-uwp build: folder with include/, SDL3.lib and SDL3.dll (RTCW_UWP only)")

set(PRODUCT_VERSION "5.4")
include(cmake/Sources.cmake)

find_package(FFMPEG REQUIRED)

# ---------------------------------------------------------------------------
# Common settings
# ---------------------------------------------------------------------------
set(COMMON_DEFS
  NDEBUG NO_GZIP USE_INTERNAL_JPEG BUILD_FREETYPE FT2_BUILD_LIBRARY
  USE_LOCAL_HEADERS "PRODUCT_VERSION=\"${PRODUCT_VERSION}\"" ARCH_64 idx64
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _WINSOCK_DEPRECATED_NO_WARNINGS
)
set(COMMON_INCS
  ${CMAKE_SOURCE_DIR}/code/zlib-1.2.11
  ${CMAKE_SOURCE_DIR}/code/jpeg-8c
  ${CMAKE_SOURCE_DIR}/code/freetype-2.9/include
)
# /W3 rather than the project's -Wall: MSVC's /Wall is extremely noisy.
add_compile_options(/W3 /MP /utf-8 /wd4996 /wd4244 /wd4267 /wd4305 /wd4018)
if(RTCW_UWP)
  # Static CRT: an AppContainer process can't load the desktop vcruntime DLLs.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(NOT EXISTS "${SDL3_UWP_DIR}/SDL3.lib")
    message(FATAL_ERROR "RTCW_UWP needs SDL3_UWP_DIR pointing at an SDL3-uwp build (missing ${SDL3_UWP_DIR}/SDL3.lib)")
  endif()
  set(SDL3_INC "${SDL3_UWP_DIR}/include")
  set(SDL3_LIB "${SDL3_UWP_DIR}/SDL3.lib")
else()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  set(SDL3_INC "${CMAKE_SOURCE_DIR}/code/SDL3/include")
  set(SDL3_LIB "${CMAKE_SOURCE_DIR}/code/libs/win64/SDL3.lib")
endif()

# ---------------------------------------------------------------------------
# Client executable (engine + statically linked OpenGL renderer)
# ---------------------------------------------------------------------------
set(MASM_SRC
  code/asm/ftola.asm
  code/asm/snapvector.asm
  code/asm/vm_x86_64.asm
)
set_source_files_properties(${MASM_SRC} PROPERTIES COMPILE_OPTIONS "/Didx64")

if(RTCW_UWP)
  set(PLATFORM_SRC code/sys/sys_uwp.c code/sys/uwp_display.cpp)
  # C++/WinRT (TV refresh rate switching)
  set_source_files_properties(code/sys/uwp_display.cpp PROPERTIES
    COMPILE_OPTIONS "/std:c++17;/EHsc;/permissive-;/bigobj")
  set(_cppwinrt "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
  if(EXISTS "${_cppwinrt}")
    set_property(SOURCE code/sys/uwp_display.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${_cppwinrt}")
  endif()
  # No OpenAL (desktop audio backend) or Mumble on Xbox; SDL handles audio.
  set(PLATFORM_DEFS RTCW_UWP)
else()
  set(PLATFORM_SRC code/sys/win_resource.rc)
  set(PLATFORM_DEFS USE_OPENAL USE_OPENAL_DLOPEN USE_MUMBLE USE_ICON)
endif()

add_executable(realrtcw WIN32
  ${CLIENT_CORE_SRC} ${BOTLIB_SRC} ${CODEC_SRC} ${ZLIB_SRC}
  ${RENDERER_SRC} ${JPEG_SRC} ${FREETYPE_SRC}
  ${MASM_SRC}
  ${PLATFORM_SRC}
)
set_target_properties(realrtcw PROPERTIES OUTPUT_NAME "RealRTCW.x64")
target_compile_definitions(realrtcw PRIVATE ${COMMON_DEFS} ${PLATFORM_DEFS}
  USE_VOIP USE_BLOOM
  USE_CODEC_OPUS OPUS_BUILD HAVE_LRINTF FLOATING_POINT FLOAT_APPROX USE_ALLOCA
  USE_CODEC_VORBIS
)
set_source_files_properties(${BOTLIB_SRC} PROPERTIES COMPILE_DEFINITIONS BOTLIB)
target_include_directories(realrtcw PRIVATE ${COMMON_INCS}
  ${SDL3_INC}
  ${CMAKE_SOURCE_DIR}/code/opus-1.2.1/include
  ${CMAKE_SOURCE_DIR}/code/opus-1.2.1/celt
  ${CMAKE_SOURCE_DIR}/code/opus-1.2.1/silk
  ${CMAKE_SOURCE_DIR}/code/opus-1.2.1/silk/float
  ${CMAKE_SOURCE_DIR}/code/opusfile-0.9/include
  ${CMAKE_SOURCE_DIR}/code/libvorbis-1.3.6/include
  ${CMAKE_SOURCE_DIR}/code/libvorbis-1.3.6/lib
  ${CMAKE_SOURCE_DIR}/code/libogg-1.3.3/include
  ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(realrtcw PRIVATE
  ${SDL3_LIB}
  ${FFMPEG_LIBRARIES}
  ws2_32 winmm psapi gdi32 ole32
)
if(RTCW_UWP)
  # OpenGL comes from Mesa's opengl32.dll in the package, loaded by SDL at
  # runtime. WinMain lives in sys_uwp.c. The exe must be flagged AppContainer.
  # 16 MB main-thread stack: the game runs on the UI thread, and the engine plus
  # Mesa's texture upload path need far more than the 1 MB default.
  target_link_libraries(realrtcw PRIVATE windowsapp)
  target_link_options(realrtcw PRIVATE /MANIFEST:NO /APPCONTAINER /STACK:16777216
    /MAP:$<TARGET_FILE_DIR:realrtcw>/RealRTCW.x64.map)
else()
  target_link_libraries(realrtcw PRIVATE opengl32)
  # win_resource.rc already embeds win_manifest.xml.
  # GUI subsystem but standard main() entry point (SDL3 doesn't supply WinMain here).
  target_link_options(realrtcw PRIVATE /MANIFEST:NO /ENTRY:mainCRTStartup)
endif()

# ---------------------------------------------------------------------------
# Game modules (loaded at runtime from main/)
# ---------------------------------------------------------------------------
function(rtcw_module name srcs defs)
  add_library(${name} SHARED ${srcs})
  target_compile_definitions(${name} PRIVATE ${COMMON_DEFS} ${defs})
  target_include_directories(${name} PRIVATE ${COMMON_INCS})
  target_link_libraries(${name} PRIVATE ws2_32 winmm psapi)
  if(RTCW_UWP)
    target_compile_definitions(${name} PRIVATE RTCW_UWP)
    target_link_options(${name} PRIVATE /APPCONTAINER)
  endif()
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${name}_sp_x64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/main"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out/main"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/out/main")
endfunction()

rtcw_module(cgame  "${CGAME_SRC}" "CGAME;CGAMEDLL")
rtcw_module(qagame "${GAME_SRC}"  "GAMEDLL;QAGAME")
rtcw_module(ui     "${UI_SRC}"    "UI")

set_target_properties(realrtcw PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/out")

# Ship the runtime DLLs next to the exe.
if(RTCW_UWP)
  add_custom_command(TARGET realrtcw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${SDL3_UWP_DIR}/SDL3.dll
      $<TARGET_FILE_DIR:realrtcw>)
else()
  add_custom_command(TARGET realrtcw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_SOURCE_DIR}/code/libs/win64/SDL3.dll
      ${CMAKE_SOURCE_DIR}/code/libs/win64/OpenAL64.dll
      $<TARGET_FILE_DIR:realrtcw>)
endif()
