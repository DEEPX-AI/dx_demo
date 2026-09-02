cmake_minimum_required(VERSION 3.14)
project(yolo_multi_demo DESCRIPTION "DEEPX YOLO Multi-Channel Object Detection Demo")

add_compile_definitions(PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}")

# common_util.hpp uses std::filesystem; MSVC has no <experimental/filesystem>,
# so C++17 is required (GCC/Clang are happy with it too).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_DEBUG_INFO "Include debugging informations in build output" ON)
option(ENABLE_AVX2 "Enable AVX2 SIMD instructions for Release build (requires CPU support)" ON)
option(ENABLE_LTO  "Enable Link Time Optimization / Whole Program Optimization for Release build" ON)

# ---------------------------------------------------------------------------
# Path inputs (DEEPX_SDK_DIR, OpenCV_DIR)
#
# Both may come from a -D cache variable or the environment, and may be given
# as relative paths. normalize_path() turns them into absolute, CMake-style
# paths resolved against the directory CMake was invoked from.
# ---------------------------------------------------------------------------
function(resolve_dir var envname)
    if(NOT ${var} AND DEFINED ENV{${envname}})
        set(${var} "$ENV{${envname}}")
    endif()
    if(${var})
        file(TO_CMAKE_PATH "${${var}}" _p)
        get_filename_component(_p "${_p}" ABSOLUTE)
        set(${var} "${_p}" PARENT_SCOPE)
    endif()
endfunction()

resolve_dir(DEEPX_SDK_DIR DEEPX_SDK_DIR)
resolve_dir(OpenCV_DIR OpenCV_DIR)

# ---------------------------------------------------------------------------
# Compiler / platform options
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W3 /MP /bigobj /utf-8 /EHsc)
    # NOMINMAX : Windows.h min/max macros collide with std::min/std::max.
    # NOGDI    : <wingdi.h> defines `ERROR`, colliding with dxrt's enum LEVEL::ERROR
    #            (the demo uses no GDI). See also common_util.hpp's #undef ERROR.
    add_compile_definitions(NOMINMAX NOGDI WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)

    # --- Release optimizations ---
    # /Gy : Function-Level Linking  (dead-code strip by linker)
    # /Oi : Enable intrinsic functions  (e.g. memcpy, sqrt -> inline)
    # /Ot : Favor fast code over small code
    # /fp:fast : Relaxed IEEE754 – allows SIMD vectorization of FP loops
    # /GL : Whole-Program Optimization (pairs with /LTCG at link time)
    add_compile_options(
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/fp:fast>
    )
    if(ENABLE_AVX2)
        add_compile_options($<$<CONFIG:Release>:/arch:AVX2>)
    endif()
    if(ENABLE_LTO)
        add_compile_options($<$<CONFIG:Release>:/GL>)
        add_link_options($<$<CONFIG:Release>:/LTCG:incremental>)
    endif()

    if(ENABLE_DEBUG_INFO)
        add_compile_options($<$<CONFIG:Debug>:/Zi>)
    endif()
else()
    add_compile_options(-W -Wall -pthread -fstrict-volatile-bitfields -fPIC -march=native)

    # --- Release optimizations ---
    # -ffast-math     : Allows FP reassociation / SIMD vectorization
    # -funroll-loops  : Unroll small loops (helps inference post-processing)
    # -fomit-frame-pointer : Free up one register in hot paths
    add_compile_options(
        $<$<CONFIG:Release>:-ffast-math>
        $<$<CONFIG:Release>:-funroll-loops>
        $<$<CONFIG:Release>:-fomit-frame-pointer>
    )

    if(ENABLE_DEBUG_INFO)
        add_compile_options(-g -rdynamic)
    endif()
endif()

if(ENABLE_DEBUG_INFO)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# GStreamer VAAPI is a Linux-only acceleration path.
if(UNIX)
    execute_process(
        COMMAND gst-inspect-1.0 vaapidecodebin
        RESULT_VARIABLE GST_VAAPI_FOUND
        OUTPUT_QUIET ERROR_QUIET
    )
    if(GST_VAAPI_FOUND EQUAL 0)
        add_definitions(-DUSE_VAAPI)
    endif()
endif()

# ---------------------------------------------------------------------------
# dxrt (DEEPX runtime)
#   * Windows : ships in the DEEPX SDK ->  include/, lib/x64/dxrt.lib, bin/x64/*.dll
#   * Linux   : debian package / manual install prefix (DXRT_INSTALLED_DIR)
# The debian package ships no CMake package config, so we locate it directly.
# ---------------------------------------------------------------------------
if(WIN32)
    if(NOT DEEPX_SDK_DIR)
        message(FATAL_ERROR
            "DEEPX_SDK_DIR is not set. Point it at the DEEPX SDK install directory "
            "via the environment (set DEEPX_SDK_DIR=...) or -DDEEPX_SDK_DIR=<path>.")
    endif()
    find_path(DXRT_INCLUDE_DIR dxrt/dxrt_api.h
        HINTS "${DEEPX_SDK_DIR}/include" NO_DEFAULT_PATH)
    find_library(DXRT_LIBRARY dxrt
        HINTS "${DEEPX_SDK_DIR}/lib/x64" "${DEEPX_SDK_DIR}/lib" NO_DEFAULT_PATH)
else()
    find_library(DXRT_LIBRARY dxrt HINTS ${DXRT_INSTALLED_DIR}/lib)
    find_path(DXRT_INCLUDE_DIR dxrt/dxrt_api.h HINTS ${DXRT_INSTALLED_DIR}/include)
endif()

if(NOT DXRT_LIBRARY OR NOT DXRT_INCLUDE_DIR)
    message(FATAL_ERROR
        "dxrt not found. On Windows set DEEPX_SDK_DIR; on Linux install dx_rt "
        "or set -DDXRT_INSTALLED_DIR=<install prefix>.")
endif()

# OpenCV from vcpkg pulls in transitive find_dependency() calls (Protobuf, etc.)
# whose CMake configs live under the vcpkg install prefix. OpenCV_DIR points at
# <vcpkg-installed>/<triplet>/share/opencv4, so its grandparent is the prefix to
# add to CMAKE_PREFIX_PATH for those dependencies to resolve.
if(OpenCV_DIR)
    get_filename_component(_vcpkg_prefix "${OpenCV_DIR}/../.." ABSOLUTE)
    if(EXISTS "${_vcpkg_prefix}/share")
        list(APPEND CMAKE_PREFIX_PATH "${_vcpkg_prefix}")
    endif()
endif()

find_package(OpenCV REQUIRED)
find_package(OpenMP)

# Check for OpenCV freetype contrib module
if(TARGET opencv_freetype OR OpenCV_freetype_FOUND OR "opencv_freetype" IN_LIST OpenCV_LIBS)
  set(HAVE_OPENCV_FREETYPE TRUE)
else()
  find_library(_opencv_freetype_lib NAMES opencv_freetype PATHS ${OpenCV_INSTALL_PATH}/lib NO_DEFAULT_PATH)
  if(_opencv_freetype_lib)
    set(HAVE_OPENCV_FREETYPE TRUE)
  endif()
endif()
if(NOT HAVE_OPENCV_FREETYPE)
  find_package(Freetype)
  if(Freetype_FOUND)
    set(HAVE_FREETYPE_DIRECT TRUE)
  endif()
endif()

set(DEMO_UTILS_DIR ${CMAKE_SOURCE_DIR}/src/demo_utils)
set(DEMO_UTILS_SRC
    ${DEMO_UTILS_DIR}/yolo_cfg.cpp
    ${DEMO_UTILS_DIR}/yolo.cpp
    ${DEMO_UTILS_DIR}/nms.cpp
    ${DEMO_UTILS_DIR}/bbox.cpp
    ${DEMO_UTILS_DIR}/image.cpp
    ${DEMO_UTILS_DIR}/display.cpp
)

add_executable(yolo_multi_demo
    ${DEMO_UTILS_SRC}
    ${CMAKE_SOURCE_DIR}/src/profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/od.cpp
    ${CMAKE_SOURCE_DIR}/src/yolo_multi_channels.cpp
)

target_include_directories(yolo_multi_demo PRIVATE
    ${DEMO_UTILS_DIR}/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/extern/
    ${CMAKE_SOURCE_DIR}/lib/
    ${DXRT_INCLUDE_DIR}
)

target_link_libraries(yolo_multi_demo PRIVATE ${DXRT_LIBRARY} ${OpenCV_LIBS})
if(NOT WIN32)
    target_link_libraries(yolo_multi_demo PRIVATE pthread stdc++fs)
else()
    # psapi : GetProcessMemoryInfo, for the profiler's RSS / page-fault counters
    # winmm : timeBeginPeriod, to move the 15.6 ms timer tick down to 1 ms
    target_link_libraries(yolo_multi_demo PRIVATE psapi winmm)
endif()
if(OpenMP_CXX_FOUND)
    target_link_libraries(yolo_multi_demo PRIVATE OpenMP::OpenMP_CXX)
endif()

if(HAVE_OPENCV_FREETYPE)
  target_compile_definitions(yolo_multi_demo PRIVATE HAVE_OPENCV_FREETYPE)
endif()
if(HAVE_FREETYPE_DIRECT)
  target_compile_definitions(yolo_multi_demo PRIVATE HAVE_FREETYPE_DIRECT)
  target_link_libraries(yolo_multi_demo PRIVATE Freetype::Freetype)
endif()

# ---------------------------------------------------------------------------
# Windows: stage runtime DLLs next to the .exe so it runs from the build tree.
# ---------------------------------------------------------------------------
if(WIN32)
    if(EXISTS "${DEEPX_SDK_DIR}/bin/x64")
        add_custom_command(TARGET yolo_multi_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${DEEPX_SDK_DIR}/bin/x64" "$<TARGET_FILE_DIR:yolo_multi_demo>"
            COMMENT "Copying DEEPX runtime DLLs from ${DEEPX_SDK_DIR}/bin/x64")
    endif()
    # OpenCV DLLs (vcpkg lays them out at <root>/bin relative to share/opencv4).
    set(_ocv_bin "")
    if(OpenCV_DIR)
        get_filename_component(_ocv_bin "${OpenCV_DIR}/../../bin" ABSOLUTE)
        if(EXISTS "${_ocv_bin}")
            add_custom_command(TARGET yolo_multi_demo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_ocv_bin}" "$<TARGET_FILE_DIR:yolo_multi_demo>"
                COMMENT "Copying OpenCV runtime DLLs from ${_ocv_bin}")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# IPO / LTO  (GCC/Clang: -flto;  MSVC: handled above via /GL + /LTCG)
# ---------------------------------------------------------------------------
if(ENABLE_LTO AND NOT MSVC)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _IPO_OK OUTPUT _IPO_ERR)
    if(_IPO_OK)
        set_property(TARGET yolo_multi_demo PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        message(STATUS "LTO enabled for Release")
    else()
        message(STATUS "LTO not supported: ${_IPO_ERR}")
    endif()
endif()

install(TARGETS yolo_multi_demo DESTINATION bin)

# On Windows the installed .exe needs its runtime DLLs alongside it in bin/.
if(WIN32)
    if(EXISTS "${DEEPX_SDK_DIR}/bin/x64")
        install(DIRECTORY "${DEEPX_SDK_DIR}/bin/x64/" DESTINATION bin
                FILES_MATCHING PATTERN "*.dll")
    endif()
    if(_ocv_bin AND EXISTS "${_ocv_bin}")
        install(DIRECTORY "${_ocv_bin}/" DESTINATION bin
                FILES_MATCHING PATTERN "*.dll")
    endif()
endif()
