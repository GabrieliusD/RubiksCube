cmake_minimum_required(VERSION 3.22.1)

project(Rubik LANGUAGES CXX)

# A game module: implements IGameModule and exports CreateGameModule().
# Game assets live in Content/ and are copied next to the launcher.
set(RubikSrc
    RubikModule.cpp
    RubikModule.h
    RubikPuzzle.cpp
    RubikPuzzle.h
)

if(CHERRY_MONOLITHIC)
    add_library(RubikGame STATIC ${RubikSrc})
else()
    add_library(RubikGame SHARED ${RubikSrc})
endif()

target_link_libraries(RubikGame PRIVATE CherryEngine)

if(MSVC)
    target_compile_options(RubikGame PRIVATE /W3 /utf-8)
endif()

# Puzzle logic tests (pure C++, no engine): ctest -C Debug
add_executable(RubikPuzzleTests Tests/RubikPuzzleTests.cpp RubikPuzzle.cpp)
add_test(NAME RubikPuzzleTests COMMAND RubikPuzzleTests)
