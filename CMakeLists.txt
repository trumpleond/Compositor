cmake_minimum_required(VERSION 3.24)

project(Compositor LANGUAGES C)

option(COMPOSITOR_BUILD_PIXEL_CORE "Build the platform-independent pixel-processing core" ON)
option(COMPOSITOR_ENABLE_OPENMP "Enable OpenMP for row-parallel pixel operations" ON)

if(NOT COMPOSITOR_BUILD_PIXEL_CORE)
    message(FATAL_ERROR "The Windows build currently requires COMPOSITOR_BUILD_PIXEL_CORE=ON")
endif()

set(COMPOSITOR_PIXEL_SOURCES
    Compositor/Rendering/AdjustPixels.c
    Compositor/Rendering/BrushPixels.c
    Compositor/Rendering/ContentFill.c
    Compositor/Rendering/DitherPixels.c
    Compositor/Rendering/HealPixels.c
    Compositor/Rendering/LevelsPixels.c
    Compositor/Rendering/LensPixels.c
    Compositor/Rendering/NoisePixels.c
    Compositor/Rendering/WandPixels.c
)

add_library(compositor_pixel_core STATIC ${COMPOSITOR_PIXEL_SOURCES})
target_include_directories(compositor_pixel_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Compositor/Rendering
)
target_compile_features(compositor_pixel_core PUBLIC c_std_11)

if(MSVC)
    target_compile_definitions(compositor_pixel_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(compositor_pixel_core PRIVATE /W4 /O2)
else()
    target_compile_options(compositor_pixel_core PRIVATE -Wall -Wextra -Wpedantic -O3)
endif()

if(COMPOSITOR_ENABLE_OPENMP)
    find_package(OpenMP QUIET)
    if(OpenMP_C_FOUND)
        target_link_libraries(compositor_pixel_core PUBLIC OpenMP::OpenMP_C)
        target_compile_definitions(compositor_pixel_core PUBLIC COMPOSITOR_HAS_OPENMP=1)
    else()
        message(STATUS "OpenMP was not found; using the serial pixel-core fallback")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(compositor_pixel_core_smoke tests/pixel_core_smoke.c)
    target_link_libraries(compositor_pixel_core_smoke PRIVATE compositor_pixel_core)
    add_test(NAME compositor_pixel_core_smoke COMMAND compositor_pixel_core_smoke)
endif()
