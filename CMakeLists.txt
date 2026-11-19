cmake_minimum_required(VERSION 3.25)

project(Oracis VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ORACIS_BUILD_HOST "Build the SDL3 desktop/Android host" ON)
option(ORACIS_FETCH_SDL3 "Download and build the pinned SDL3 release" ON)

if(ORACIS_BUILD_HOST)
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
endif()

# C-compatible include surface consumed by standard generated C banks.
add_library(oracis_generated_bank_abi INTERFACE)
target_include_directories(oracis_generated_bank_abi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/oracis/recomp
)
target_compile_features(oracis_generated_bank_abi INTERFACE c_std_99)

# The runtime is part of the product regardless of whether tests are enabled.
add_library(oracis_arm_state STATIC
    src/runtime/arm_cpu_state_ops.cpp
    src/runtime/arm_psr_ops.cpp
    src/runtime/arm_exception_ops.cpp
    src/runtime/arm_flags.cpp
    src/runtime/recomp_abi.cpp
)
target_link_libraries(oracis_arm_state PUBLIC oracis_generated_bank_abi)
target_compile_features(oracis_arm_state PUBLIC cxx_std_20)

add_library(oracis_memory STATIC
    src/memory/guest_memory.cpp
    src/memory/bus_abi.cpp
)
target_link_libraries(oracis_memory PUBLIC
    oracis_generated_bank_abi
    oracis_arm_state
)
target_compile_features(oracis_memory PUBLIC cxx_std_20)

# The SDL host consumes the same independent runtime library that generated
# static banks and the cooperative scheduler will use.
if(ORACIS_BUILD_HOST)
    target_link_libraries(${ORACIS_RUNTIME_TARGET} PRIVATE
        oracis_memory
    )
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(oracis_guest_memory_test
        tests/memory/guest_memory_test.cpp
    )
    target_link_libraries(oracis_guest_memory_test PRIVATE oracis_memory)
    add_test(NAME oracis_guest_memory_test COMMAND oracis_guest_memory_test)

    add_executable(oracis_guest_memory_bus_abi_test
        tests/memory/guest_memory_bus_abi_test.cpp
    )
    target_link_libraries(oracis_guest_memory_bus_abi_test PRIVATE
        oracis_memory
    )
    add_test(NAME oracis_guest_memory_bus_abi_test COMMAND oracis_guest_memory_bus_abi_test)

    add_executable(oracis_abi_c_test
        tests/abi/arm_cpu_state_c_test.c
    )
    target_include_directories(oracis_abi_c_test PRIVATE include)
    target_compile_features(oracis_abi_c_test PRIVATE c_std_99)
    add_test(NAME oracis_abi_c_test COMMAND oracis_abi_c_test)

    add_executable(oracis_abi_cpp_test
        tests/abi/arm_cpu_state_cpp_test.cpp
    )
    target_include_directories(oracis_abi_cpp_test PRIVATE include)
    target_compile_features(oracis_abi_cpp_test PRIVATE cxx_std_20)
    add_test(NAME oracis_abi_cpp_test COMMAND oracis_abi_cpp_test)

    add_executable(oracis_arm_state_ops_test
        tests/abi/arm_cpu_state_ops_test.cpp
    )
    target_link_libraries(oracis_arm_state_ops_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_arm_state_ops_test COMMAND oracis_arm_state_ops_test)

    add_executable(oracis_arm_psr_ops_test
        tests/abi/arm_psr_ops_test.cpp
    )
    target_link_libraries(oracis_arm_psr_ops_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_arm_psr_ops_test COMMAND oracis_arm_psr_ops_test)

    add_executable(oracis_arm_exception_ops_test
        tests/abi/arm_exception_ops_test.cpp
    )
    target_link_libraries(oracis_arm_exception_ops_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_arm_exception_ops_test COMMAND oracis_arm_exception_ops_test)

    add_executable(oracis_arm_flags_test
        tests/abi/arm_flags_test.cpp
    )
    target_link_libraries(oracis_arm_flags_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_arm_flags_test COMMAND oracis_arm_flags_test)

    add_executable(oracis_runtime_abi_bridge_test
        tests/abi/runtime_abi_bridge_test.cpp
    )
    target_link_libraries(oracis_runtime_abi_bridge_test PRIVATE oracis_arm_state)
    target_include_directories(oracis_runtime_abi_bridge_test PRIVATE include/oracis/recomp)
    add_test(NAME oracis_runtime_abi_bridge_test COMMAND oracis_runtime_abi_bridge_test)

    add_executable(oracis_runtime_abi_bridge_c_test
        tests/abi/runtime_abi_bridge_c_test.c
    )
    target_link_libraries(oracis_runtime_abi_bridge_c_test PRIVATE oracis_arm_state)
    target_include_directories(oracis_runtime_abi_bridge_c_test PRIVATE include)
    target_compile_features(oracis_runtime_abi_bridge_c_test PRIVATE c_std_99)
    set_target_properties(oracis_runtime_abi_bridge_c_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME oracis_runtime_abi_bridge_c_test COMMAND oracis_runtime_abi_bridge_c_test)

    add_executable(oracis_static_arm9_abi_test
        tests/abi/runtime_abi_static_cpu_test.c
    )
    target_link_libraries(oracis_static_arm9_abi_test PRIVATE oracis_generated_bank_abi)
    target_compile_definitions(oracis_static_arm9_abi_test PRIVATE NDS_STATIC_CPU=0)
    add_test(NAME oracis_static_arm9_abi_test COMMAND oracis_static_arm9_abi_test)

    add_executable(oracis_static_arm7_abi_test
        tests/abi/runtime_abi_static_cpu_test.c
    )
    target_link_libraries(oracis_static_arm7_abi_test PRIVATE oracis_generated_bank_abi)
    target_compile_definitions(oracis_static_arm7_abi_test PRIVATE NDS_STATIC_CPU=1)
    add_test(NAME oracis_static_arm7_abi_test COMMAND oracis_static_arm7_abi_test)
endif()
