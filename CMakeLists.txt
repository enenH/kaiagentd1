cmake_minimum_required(VERSION 3.22.1)
if (NOT DEFINED CMAKE_ANDROID_NDK)
    if (DEFINED ANDROID_NDK)
        set(CMAKE_ANDROID_NDK "${ANDROID_NDK}")
    elseif (DEFINED ENV{ANDROID_NDK_HOME})
        set(CMAKE_ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
    elseif (DEFINED ENV{ANDROID_NDK_ROOT})
        set(CMAKE_ANDROID_NDK "$ENV{ANDROID_NDK_ROOT}")
    endif ()
endif ()
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (NOT DEFINED CMAKE_ANDROID_NDK)
        message(FATAL_ERROR "Set CMAKE_ANDROID_NDK, ANDROID_NDK_HOME, or CMAKE_TOOLCHAIN_FILE to an Android NDK installation.")
    endif ()
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
endif ()
if (NOT DEFINED CMAKE_SYSTEM_NAME)
    set(CMAKE_SYSTEM_NAME Android)
endif ()
if (NOT DEFINED CMAKE_SYSTEM_VERSION)
    set(CMAKE_SYSTEM_VERSION 34)
endif ()
if (NOT DEFINED CMAKE_ANDROID_ARCH_ABI AND NOT DEFINED ANDROID_ABI)
    set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
    set(ANDROID_ABI arm64-v8a)
elseif (DEFINED CMAKE_ANDROID_ARCH_ABI AND NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI ${CMAKE_ANDROID_ARCH_ABI})
elseif (DEFINED ANDROID_ABI AND NOT DEFINED CMAKE_ANDROID_ARCH_ABI)
    set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
endif ()
if (NOT DEFINED ANDROID_PLATFORM)
    set(ANDROID_PLATFORM android-${CMAKE_SYSTEM_VERSION})
endif ()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (DEFINED CMAKE_ANDROID_NDK)
    set(ANDROID_NDK "${CMAKE_ANDROID_NDK}")
endif ()
project(kaiagentd)


# Position-independent code and section-level dead-code elimination.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -ffunction-sections -fdata-sections")

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto -fvisibility=hidden -O3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fvisibility=hidden -fvisibility-inlines-hidden -O3")
    add_compile_definitions(NDEBUG)
    set(RELEASE_LINKER_FLAGS "-flto -Wl,--wrap=strlen -Wl,--wrap=__strlen_chk -Wl,--wrap=strcpy -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${RELEASE_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${RELEASE_LINKER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${RELEASE_LINKER_FLAGS}")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g")
endif ()

include(FetchContent)
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-14-3
)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(freetype)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")



add_executable(
    kaiagentd
    "${SRC_DIR}/launch_args.cpp"
    "${SRC_DIR}/font.cpp"
    "${SRC_DIR}/main.cpp"
    "${SRC_DIR}/jvm_runtime.cpp"
    "${SRC_DIR}/menu_ui.cpp"
    "${SRC_DIR}/overlay_canvas.cpp"
    "${SRC_DIR}/overlay_draw.cpp"
    "${SRC_DIR}/overlay_surface.cpp"
    "${SRC_DIR}/raw_capture.cpp"
    "${SRC_DIR}/touchproxy.cpp"
    "${SRC_DIR}/wrap_string.c"
)

set_target_properties(
    kaiagentd
    PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "kaiagentd"
        SUFFIX ".so"
)

target_compile_features(kaiagentd PRIVATE cxx_std_20)
target_include_directories(kaiagentd PRIVATE "${INCLUDE_DIR}")
target_link_libraries(kaiagentd PRIVATE android log dl mediandk freetype)
