cmake_minimum_required(VERSION 3.22.1)

project(Dice LANGUAGES CXX)

# The dice roll feel prototype (PROTOTYPE_EXPORT.md), ported to CherryEngine.
# DiceSim and DiceVisuals are pure C++ (the tests link them without the engine's device).
set(DiceCore
    DiceConfig.h
    DiceControls.cpp
    DiceControls.h
    DiceMath.h
    DiceSim.cpp
    DiceSim.h
    DiceVisuals.cpp
    DiceVisuals.h
)
set(DiceSrc
    ${DiceCore}
    DiceAudio.cpp
    DiceAudio.h
    DiceHud.cpp
    DiceModule.cpp
    DiceModule.h
    DicePanel.cpp
)

if(CHERRY_MONOLITHIC)
    add_library(DiceGame STATIC ${DiceSrc})
else()
    add_library(DiceGame SHARED ${DiceSrc})
endif()

target_link_libraries(DiceGame PRIVATE CherryEngine)
cherry_register_game_module(DiceGame)

if(MSVC)
    target_compile_options(DiceGame PRIVATE /W3 /utf-8)
endif()

if(ANDROID)
    return()   # the tests and the benchmark run on the build machine
endif()

# Face mapping, tumble direction, board and (from M3 on) the B11 invariants: ctest -C Debug -R DiceTests
add_executable(DiceTests Tests/DiceTests.cpp ${DiceCore} DiceAudio.cpp)
target_link_libraries(DiceTests PRIVATE CherryEngine)
if(MSVC)
    target_compile_options(DiceTests PRIVATE /W3 /utf-8)
endif()
# Simulation cost at 10k / 50k / 100k dice (M10): run it by hand, it's not a test.
add_executable(DiceBench Tests/DiceBench.cpp ${DiceCore})
target_link_libraries(DiceBench PRIVATE CherryEngine)

target_compile_definitions(DiceTests PRIVATE DICE_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Content")
add_test(NAME DiceTests COMMAND DiceTests)
