cmake_minimum_required(VERSION 3.20)
project(hmfomt_pc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL2 REQUIRED)

add_library(hmfomt_core STATIC
    src/cpu/arm7tdmi.cpp
    src/cpu/gba_bios_hle.cpp
    src/hal/gba_bus.cpp
    src/hal/hal_audio.cpp
    src/hal/hal_input.cpp
    src/hal/hal_save.cpp
    src/hal/hal_timing.cpp
    src/hal/hal_video.cpp
    src/ppu/ppu.cpp
    src/game/fomt_domain.cpp
    src/game/fomt_rom_assets.cpp
    src/game/ewram_coop_bridge.cpp
    src/game/gba_scene_assets.cpp
    src/game/game_engine.cpp
    src/game/script_vm.cpp
    src/net/net_udp.cpp
    src/net/net_coop.cpp
)

target_include_directories(hmfomt_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/fomt
)

target_compile_definitions(hmfomt_core PUBLIC SDL_MAIN_HANDLED)

if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hmfomt_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(hmfomt_core
    PUBLIC
        SDL2::SDL2
        $<$<PLATFORM_ID:Windows>:ws2_32>
)

add_executable(hmfomt_pc src/main.cpp)
target_link_libraries(hmfomt_pc PRIVATE hmfomt_core)

enable_testing()

function(add_hm_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE hmfomt_core)
    if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -UNDEBUG)
    elseif(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_hm_test(test_hal_bus tests/test_hal_bus.cpp)
add_hm_test(test_arm7tdmi tests/test_arm7tdmi.cpp)
add_hm_test(test_ppu tests/test_ppu.cpp)
add_hm_test(test_game_domain tests/test_game_domain.cpp)
add_hm_test(test_rom_assets tests/test_rom_assets.cpp)
add_hm_test(test_rom_boot tests/test_rom_boot.cpp)
add_hm_test(test_net_coop tests/test_net_coop.cpp)
add_hm_test(test_ewram_coop tests/test_ewram_coop.cpp)
