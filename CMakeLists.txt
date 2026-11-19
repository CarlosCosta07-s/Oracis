cmake_minimum_required(VERSION 3.25)

project(Oracis VERSION 0.1.0 LANGUAGES C)

include(FetchContent)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ORACIS_FETCH_SDL3 "Download and build the pinned SDL3 release" ON)

if(ORACIS_FETCH_SDL3)
    # Pin the dependency and verify the downloaded source archive.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SDL3)
else()
    find_package(SDL3 3.4.20 CONFIG REQUIRED)
endif()

if(ANDROID)
    # SDL's Android Activity loads a shared library named "main".
    add_library(main SHARED src/main.c)
    set(ORACIS_RUNTIME_TARGET main)
else()
    add_executable(oracis src/main.c)
    set(ORACIS_RUNTIME_TARGET oracis)
endif()

target_link_libraries(${ORACIS_RUNTIME_TARGET} PRIVATE SDL3::SDL3)

target_compile_features(${ORACIS_RUNTIME_TARGET} PRIVATE c_std_99)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${ORACIS_RUNTIME_TARGET} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
