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
set(ORACIS_LOCAL_BANK_DIR "" CACHE PATH
    "Optional local directory containing emitted Platinum ARM9/ARM7 C banks")
option(ORACIS_ALLOW_SYNTHETIC_BANKS
    "Allow explicitly marked synthetic bank fixtures (tests only)" OFF)
set(ORACIS_BANK_SYMBOL_ALLOWLIST "" CACHE FILEPATH
    "Optional local allowlist for verified platform/C-runtime bank symbols")

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
    src/runtime/static_dispatch.cpp
    src/runtime/static_dispatch_abi.cpp
    src/runtime/call_return_stack.cpp
    src/runtime/scheduler_core.cpp
    src/runtime/runtime_event_trace.cpp
    src/runtime/cycle_accounting.cpp
)
target_link_libraries(oracis_arm_state PUBLIC oracis_generated_bank_abi)
target_compile_features(oracis_arm_state PUBLIC cxx_std_20)

add_library(oracis_memory STATIC
    src/memory/guest_memory.cpp
    src/memory/nds_arm9_tcm.cpp
    src/memory/bus_abi.cpp
    src/memory/nds_retail_map.cpp
    src/memory/nds_wram_control.cpp
    src/memory/nds_vram_control.cpp
    src/memory/nds_video_memory.cpp
    src/memory/nds_post_flag.cpp
    src/memory/nds_display_control.cpp
    src/memory/nds_geometry_engine.cpp
    src/memory/nds_3d_texture_sampler.cpp
    src/memory/nds_3d_rasterizer.cpp
    src/memory/nds_3d_scanline_cache.cpp
    src/memory/nds_2d_effects.cpp
    src/memory/nds_2d_bg_fetcher.cpp
    src/memory/nds_2d_affine_bg_fetcher.cpp
    src/memory/nds_2d_bg_compositor.cpp
    src/memory/nds_2d_obj_fetcher.cpp
    src/memory/nds_interrupt_control.cpp
    src/memory/nds_ipc_control.cpp
    src/memory/nds_game_card_control.cpp
    src/memory/nds_game_card_key1.cpp
    src/memory/nds_game_card_key2.cpp
    src/memory/nds_dma_control.cpp
    src/memory/nds_timer_control.cpp
    src/memory/nds_keypad_control.cpp
    src/memory/nds_system_clock.cpp
)
target_link_libraries(oracis_memory PUBLIC
    oracis_generated_bank_abi
    oracis_arm_state
)
target_compile_features(oracis_memory PUBLIC cxx_std_20)

# Owns and binds one coherent NDS machine context over the runtime devices.
add_library(oracis_machine STATIC
    src/machine/nds_machine.cpp
    src/machine/host_runtime_bridge.cpp
    src/machine/sha1.cpp
)
target_link_libraries(oracis_machine PUBLIC oracis_memory oracis_arm_state)
target_compile_features(oracis_machine PUBLIC cxx_std_20)

# User-owned NDSRecomp output is optional and never fetched by CI. The bank
# directory must contain the exact filenames produced by emit_platinum_banks.py.
if(ORACIS_LOCAL_BANK_DIR)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    get_filename_component(ORACIS_LOCAL_BANK_DIR
        "${ORACIS_LOCAL_BANK_DIR}" ABSOLUTE)

    set(_oracis_bank_validation_args)
    if(ORACIS_ALLOW_SYNTHETIC_BANKS)
        list(APPEND _oracis_bank_validation_args --allow-synthetic-fixture)
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ndsrecomp/validate_platinum_bank_set.py"
            --bank-dir "${ORACIS_LOCAL_BANK_DIR}"
            ${_oracis_bank_validation_args}
        RESULT_VARIABLE _oracis_bank_validation_result
        OUTPUT_VARIABLE _oracis_bank_validation_stdout
        ERROR_VARIABLE _oracis_bank_validation_stderr
    )
    if(NOT _oracis_bank_validation_result EQUAL 0)
        message(FATAL_ERROR
            "Local NDSRecomp bank-set validation failed. Re-run "
            "tools/ndsrecomp/emit_platinum_banks.py with the correct user-owned "
            "USA Rev 1 image.\n"
            "${_oracis_bank_validation_stdout}"
            "${_oracis_bank_validation_stderr}")
    endif()
    message(STATUS "${_oracis_bank_validation_stdout}")

    set(ORACIS_ARM9_BANK_NAME "pokemon_platinum_usa_rev1_arm9")
    set(ORACIS_ARM7_BANK_NAME "pokemon_platinum_usa_rev1_arm7")

    foreach(bank_name IN ITEMS
            ${ORACIS_ARM9_BANK_NAME}
            ${ORACIS_ARM7_BANK_NAME})
        foreach(extension IN ITEMS ".c" ".h" "_dispatch.c")
            if(NOT EXISTS "${ORACIS_LOCAL_BANK_DIR}/${bank_name}${extension}")
                message(FATAL_ERROR
                    "Missing generated bank file: "
                    "${ORACIS_LOCAL_BANK_DIR}/${bank_name}${extension}. "
                    "Run tools/ndsrecomp/emit_platinum_banks.py first.")
            endif()
        endforeach()
    endforeach()

    add_library(oracis_platinum_arm9_bank OBJECT
        "${ORACIS_LOCAL_BANK_DIR}/${ORACIS_ARM9_BANK_NAME}.c"
        "${ORACIS_LOCAL_BANK_DIR}/${ORACIS_ARM9_BANK_NAME}_dispatch.c")
    add_library(oracis_platinum_arm7_bank OBJECT
        "${ORACIS_LOCAL_BANK_DIR}/${ORACIS_ARM7_BANK_NAME}.c"
        "${ORACIS_LOCAL_BANK_DIR}/${ORACIS_ARM7_BANK_NAME}_dispatch.c")

    foreach(bank_target IN ITEMS
            oracis_platinum_arm9_bank
            oracis_platinum_arm7_bank)
        target_link_libraries(${bank_target} PRIVATE oracis_generated_bank_abi)
        target_include_directories(${bank_target} PRIVATE
            "${ORACIS_LOCAL_BANK_DIR}")
    endforeach()

    target_sources(oracis_machine PRIVATE
        $<TARGET_OBJECTS:oracis_platinum_arm9_bank>
        $<TARGET_OBJECTS:oracis_platinum_arm7_bank>)
    target_compile_definitions(oracis_machine PRIVATE
        ORACIS_WITH_PLATINUM_STATIC_BANKS=1)
    if(ORACIS_BUILD_HOST)
        target_compile_definitions(${ORACIS_RUNTIME_TARGET} PRIVATE
            ORACIS_WITH_PLATINUM_STATIC_BANKS=1)
    endif()

    set(_oracis_bank_audit_nm_args)
    if(CMAKE_NM)
        list(APPEND _oracis_bank_audit_nm_args --nm "${CMAKE_NM}")
    endif()

    set(_oracis_bank_audit_allowlist_args)
    if(ORACIS_BANK_SYMBOL_ALLOWLIST)
        get_filename_component(ORACIS_BANK_SYMBOL_ALLOWLIST
            "${ORACIS_BANK_SYMBOL_ALLOWLIST}" ABSOLUTE)
        if(NOT EXISTS "${ORACIS_BANK_SYMBOL_ALLOWLIST}")
            message(FATAL_ERROR
                "ORACIS_BANK_SYMBOL_ALLOWLIST does not exist: "
                "${ORACIS_BANK_SYMBOL_ALLOWLIST}")
        endif()
        list(APPEND _oracis_bank_audit_allowlist_args
            --allowlist "${ORACIS_BANK_SYMBOL_ALLOWLIST}")
    endif()

    # Audit the exact objects linked into the local machine plus the symbols
    # exported by the Oracis runtime archives. This target is local-only.
    add_custom_target(oracis_audit_platinum_bank_symbols
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ndsrecomp/audit_bank_symbols.py"
            --banks
            $<TARGET_OBJECTS:oracis_platinum_arm9_bank>
            $<TARGET_OBJECTS:oracis_platinum_arm7_bank>
            --runtime
            "$<TARGET_FILE:oracis_machine>"
            "$<TARGET_FILE:oracis_memory>"
            "$<TARGET_FILE:oracis_arm_state>"
            --report-json
            "${CMAKE_CURRENT_BINARY_DIR}/oracis_platinum_bank_symbol_audit.json"
            ${_oracis_bank_audit_nm_args}
            ${_oracis_bank_audit_allowlist_args}
        COMMAND_EXPAND_LISTS
        DEPENDS
            oracis_platinum_arm9_bank
            oracis_platinum_arm7_bank
            oracis_machine
            oracis_memory
            oracis_arm_state
        VERBATIM
    )
endif()

# The SDL host consumes the independent machine/runtime libraries.
if(ORACIS_BUILD_HOST)
    target_link_libraries(${ORACIS_RUNTIME_TARGET} PRIVATE
        oracis_machine
    )
    # The SDL entry point is C, but the machine/runtime bridge is C++20.
    set_target_properties(${ORACIS_RUNTIME_TARGET} PROPERTIES
        LINKER_LANGUAGE CXX
    )
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(oracis_guest_memory_test
        tests/memory/guest_memory_test.cpp
    )
    target_link_libraries(oracis_guest_memory_test PRIVATE oracis_memory)
    add_test(NAME oracis_guest_memory_test COMMAND oracis_guest_memory_test)

    add_executable(oracis_guest_memory_generation_test
        tests/memory/guest_memory_generation_test.cpp
    )
    target_link_libraries(oracis_guest_memory_generation_test PRIVATE oracis_memory)
    add_test(NAME oracis_guest_memory_generation_test COMMAND oracis_guest_memory_generation_test)

    add_executable(oracis_nds_retail_map_test
        tests/memory/nds_retail_map_test.cpp
    )
    target_link_libraries(oracis_nds_retail_map_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_retail_map_test COMMAND oracis_nds_retail_map_test)

    add_executable(oracis_nds_wram_control_test
        tests/memory/nds_wram_control_test.cpp
    )
    target_link_libraries(oracis_nds_wram_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_wram_control_test COMMAND oracis_nds_wram_control_test)

    add_executable(oracis_nds_vram_control_test
        tests/memory/nds_vram_control_test.cpp
    )
    target_link_libraries(oracis_nds_vram_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_vram_control_test COMMAND oracis_nds_vram_control_test)

    add_executable(oracis_nds_video_memory_test
        tests/memory/nds_video_memory_test.cpp
    )
    target_link_libraries(oracis_nds_video_memory_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_video_memory_test COMMAND oracis_nds_video_memory_test)

    add_executable(oracis_nds_post_flag_test
        tests/memory/nds_post_flag_test.cpp
    )
    target_link_libraries(oracis_nds_post_flag_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_post_flag_test COMMAND oracis_nds_post_flag_test)

    add_executable(oracis_nds_display_control_test
        tests/memory/nds_display_control_test.cpp
    )
    target_link_libraries(oracis_nds_display_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_display_control_test COMMAND oracis_nds_display_control_test)

    add_executable(oracis_nds_geometry_engine_test
        tests/memory/nds_geometry_engine_test.cpp
    )
    target_link_libraries(oracis_nds_geometry_engine_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_geometry_engine_test COMMAND oracis_nds_geometry_engine_test)

    add_executable(oracis_nds_3d_texture_sampler_test
        tests/memory/nds_3d_texture_sampler_test.cpp
    )
    target_link_libraries(oracis_nds_3d_texture_sampler_test PRIVATE oracis_memory)
    add_test(
        NAME oracis_nds_3d_texture_sampler_test
        COMMAND oracis_nds_3d_texture_sampler_test
    )

    add_executable(oracis_nds_3d_rasterizer_test
        tests/memory/nds_3d_rasterizer_test.cpp
    )
    target_link_libraries(oracis_nds_3d_rasterizer_test PRIVATE oracis_memory)
    add_test(
        NAME oracis_nds_3d_rasterizer_test
        COMMAND oracis_nds_3d_rasterizer_test
    )

    add_executable(oracis_nds_3d_scanline_cache_test
        tests/memory/nds_3d_scanline_cache_test.cpp
    )
    target_link_libraries(oracis_nds_3d_scanline_cache_test PRIVATE oracis_memory)
    add_test(
        NAME oracis_nds_3d_scanline_cache_test
        COMMAND oracis_nds_3d_scanline_cache_test
    )

    add_executable(oracis_nds_3d_depth_buffer_test
        tests/memory/nds_3d_depth_buffer_test.cpp
    )
    target_link_libraries(oracis_nds_3d_depth_buffer_test PRIVATE oracis_memory)
    add_test(
        NAME oracis_nds_3d_depth_buffer_test
        COMMAND oracis_nds_3d_depth_buffer_test
    )

    add_executable(oracis_nds_3d_color_buffer_test
        tests/memory/nds_3d_color_buffer_test.cpp
    )
    target_link_libraries(oracis_nds_3d_color_buffer_test PRIVATE oracis_memory)
    add_test(
        NAME oracis_nds_3d_color_buffer_test
        COMMAND oracis_nds_3d_color_buffer_test
    )

    add_executable(oracis_nds_2d_effects_test
        tests/memory/nds_2d_effects_test.cpp
    )
    target_link_libraries(oracis_nds_2d_effects_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_2d_effects_test COMMAND oracis_nds_2d_effects_test)

    add_executable(oracis_nds_2d_bg_fetcher_test
        tests/memory/nds_2d_bg_fetcher_test.cpp
    )
    target_link_libraries(oracis_nds_2d_bg_fetcher_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_2d_bg_fetcher_test COMMAND oracis_nds_2d_bg_fetcher_test)

    add_executable(oracis_nds_2d_affine_bg_fetcher_test
        tests/memory/nds_2d_affine_bg_fetcher_test.cpp
    )
    target_link_libraries(oracis_nds_2d_affine_bg_fetcher_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_2d_affine_bg_fetcher_test COMMAND oracis_nds_2d_affine_bg_fetcher_test)

    add_executable(oracis_nds_2d_bg_compositor_test
        tests/memory/nds_2d_bg_compositor_test.cpp
    )
    target_link_libraries(oracis_nds_2d_bg_compositor_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_2d_bg_compositor_test COMMAND oracis_nds_2d_bg_compositor_test)

    add_executable(oracis_nds_2d_obj_fetcher_test
        tests/memory/nds_2d_obj_fetcher_test.cpp
    )
    target_link_libraries(oracis_nds_2d_obj_fetcher_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_2d_obj_fetcher_test COMMAND oracis_nds_2d_obj_fetcher_test)

    add_executable(oracis_nds_interrupt_control_test
        tests/memory/nds_interrupt_control_test.cpp
    )
    target_link_libraries(oracis_nds_interrupt_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_interrupt_control_test COMMAND oracis_nds_interrupt_control_test)

    add_executable(oracis_nds_ipc_control_test
        tests/memory/nds_ipc_control_test.cpp
    )
    target_link_libraries(oracis_nds_ipc_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_ipc_control_test COMMAND oracis_nds_ipc_control_test)

    add_executable(oracis_nds_game_card_key2_test
        tests/memory/nds_game_card_key2_test.cpp
    )
    target_link_libraries(oracis_nds_game_card_key2_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_game_card_key2_test COMMAND oracis_nds_game_card_key2_test)

    add_executable(oracis_nds_game_card_key1_test
        tests/memory/nds_game_card_key1_test.cpp
    )
    target_link_libraries(oracis_nds_game_card_key1_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_game_card_key1_test COMMAND oracis_nds_game_card_key1_test)

    add_executable(oracis_nds_game_card_control_test
        tests/memory/nds_game_card_control_test.cpp
    )
    target_link_libraries(oracis_nds_game_card_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_game_card_control_test COMMAND oracis_nds_game_card_control_test)

    add_executable(oracis_nds_dma_control_test
        tests/memory/nds_dma_control_test.cpp
    )
    target_link_libraries(oracis_nds_dma_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_dma_control_test COMMAND oracis_nds_dma_control_test)

    add_executable(oracis_nds_timer_control_test
        tests/memory/nds_timer_control_test.cpp
    )
    target_link_libraries(oracis_nds_timer_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_timer_control_test COMMAND oracis_nds_timer_control_test)

    add_executable(oracis_nds_keypad_control_test
        tests/memory/nds_keypad_control_test.cpp
    )
    target_link_libraries(oracis_nds_keypad_control_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_keypad_control_test COMMAND oracis_nds_keypad_control_test)

    add_executable(oracis_nds_system_clock_test
        tests/memory/nds_system_clock_test.cpp
    )
    target_link_libraries(oracis_nds_system_clock_test PRIVATE oracis_memory)
    add_test(NAME oracis_nds_system_clock_test COMMAND oracis_nds_system_clock_test)

    add_executable(oracis_guest_memory_bus_abi_test
        tests/memory/guest_memory_bus_abi_test.cpp
    )
    target_link_libraries(oracis_guest_memory_bus_abi_test PRIVATE
        oracis_memory
    )
    add_test(NAME oracis_guest_memory_bus_abi_test COMMAND oracis_guest_memory_bus_abi_test)

    add_executable(oracis_nds_machine_test
        tests/machine/nds_machine_test.cpp
    )
    target_link_libraries(oracis_nds_machine_test PRIVATE oracis_machine)
    add_test(NAME oracis_nds_machine_test COMMAND oracis_nds_machine_test)

    add_executable(oracis_host_runtime_bridge_c_test
        tests/host/runtime_bridge_c_test.c
    )
    target_link_libraries(oracis_host_runtime_bridge_c_test PRIVATE
        oracis_machine
    )
    target_compile_features(oracis_host_runtime_bridge_c_test PRIVATE c_std_99)
    set_target_properties(oracis_host_runtime_bridge_c_test PROPERTIES
        LINKER_LANGUAGE CXX
    )
    add_test(
        NAME oracis_host_runtime_bridge_c_test
        COMMAND oracis_host_runtime_bridge_c_test
    )

    add_executable(oracis_game_card_boot_loader_test
        tests/host/game_card_boot_loader_test.cpp
    )
    target_link_libraries(oracis_game_card_boot_loader_test PRIVATE
        oracis_machine
    )
    add_test(
        NAME oracis_game_card_boot_loader_test
        COMMAND oracis_game_card_boot_loader_test
    )

    add_executable(oracis_local_program_loader_test
        tests/host/local_program_loader_test.cpp
    )
    target_link_libraries(oracis_local_program_loader_test PRIVATE
        oracis_machine
    )
    add_test(
        NAME oracis_local_program_loader_test
        COMMAND oracis_local_program_loader_test
    )

    add_executable(oracis_sha1_test
        tests/host/sha1_test.cpp
    )
    target_link_libraries(oracis_sha1_test PRIVATE oracis_machine)
    add_test(NAME oracis_sha1_test COMMAND oracis_sha1_test)

    if(ORACIS_LOCAL_BANK_DIR)
        add_executable(oracis_compiled_bank_link_smoke
            tests/host/compiled_bank_link_smoke.cpp
        )
        target_link_libraries(oracis_compiled_bank_link_smoke PRIVATE
            oracis_machine
        )
        add_test(
            NAME oracis_compiled_bank_link_smoke
            COMMAND oracis_compiled_bank_link_smoke
        )
    endif()

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

    add_executable(oracis_call_return_stack_test
        tests/abi/call_return_stack_test.cpp
    )
    target_link_libraries(oracis_call_return_stack_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_call_return_stack_test COMMAND oracis_call_return_stack_test)

    add_executable(oracis_user_register_abi_test
        tests/abi/user_register_abi_test.cpp
    )
    target_link_libraries(oracis_user_register_abi_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_user_register_abi_test COMMAND oracis_user_register_abi_test)

    add_executable(oracis_runtime_scheduler_test
        tests/abi/runtime_scheduler_test.cpp
    )
    target_link_libraries(oracis_runtime_scheduler_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_runtime_scheduler_test COMMAND oracis_runtime_scheduler_test)

    add_executable(oracis_runtime_code_cycles_test
        tests/abi/runtime_code_cycles_test.cpp
    )
    target_link_libraries(oracis_runtime_code_cycles_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_runtime_code_cycles_test COMMAND oracis_runtime_code_cycles_test)

    add_executable(oracis_runtime_mul_cycles_test
        tests/abi/runtime_mul_cycles_test.cpp
    )
    target_link_libraries(oracis_runtime_mul_cycles_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_runtime_mul_cycles_test COMMAND oracis_runtime_mul_cycles_test)

    add_executable(oracis_runtime_arm7_cycle_combine_test
        tests/abi/runtime_arm7_cycle_combine_test.cpp
    )
    target_link_libraries(oracis_runtime_arm7_cycle_combine_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_runtime_arm7_cycle_combine_test COMMAND oracis_runtime_arm7_cycle_combine_test)

    add_executable(oracis_runtime_event_trace_test
        tests/abi/runtime_event_trace_test.cpp
    )
    target_link_libraries(oracis_runtime_event_trace_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_runtime_event_trace_test COMMAND oracis_runtime_event_trace_test)

    add_executable(oracis_runtime_event_trace_c_test
        tests/abi/runtime_event_trace_c_test.c
    )
    target_link_libraries(oracis_runtime_event_trace_c_test PRIVATE oracis_arm_state)
    target_compile_features(oracis_runtime_event_trace_c_test PRIVATE c_std_99)
    set_target_properties(oracis_runtime_event_trace_c_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME oracis_runtime_event_trace_c_test COMMAND oracis_runtime_event_trace_c_test)

    add_executable(oracis_static_dispatch_test
        tests/abi/static_dispatch_test.cpp
    )
    target_link_libraries(oracis_static_dispatch_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_static_dispatch_test COMMAND oracis_static_dispatch_test)

    add_executable(oracis_static_dispatch_runtime_test
        tests/abi/static_dispatch_runtime_test.cpp
    )
    target_link_libraries(oracis_static_dispatch_runtime_test PRIVATE oracis_arm_state)
    add_test(NAME oracis_static_dispatch_runtime_test COMMAND oracis_static_dispatch_runtime_test)

    add_executable(oracis_static_dispatch_abi_c_test
        tests/abi/static_dispatch_abi_c_test.c
    )
    target_link_libraries(oracis_static_dispatch_abi_c_test PRIVATE oracis_arm_state)
    target_compile_features(oracis_static_dispatch_abi_c_test PRIVATE c_std_99)
    set_target_properties(oracis_static_dispatch_abi_c_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME oracis_static_dispatch_abi_c_test COMMAND oracis_static_dispatch_abi_c_test)

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