cmake_minimum_required(VERSION 3.22.1)

project(Undertime LANGUAGES CXX)

# UNDERTIME: a game module for CherryEngine.
# Run with: CherryLaunch.exe UndertimeGame.dll
set(UndertimeSrc
    Config.h
    ColorUtil.h
    VoxelWorld.h
    VoxelWorld.cpp
    WorldCollision.h
    WorldCollision.cpp
    Fragments.h
    Fragments.cpp
    Structure.h
    Structure.cpp
    Noise.h
    CaveGenerator.h
    CaveGenerator.cpp
    CaveMap.h
    CaveMap.cpp
    Player.h
    Player.cpp
    Meshes.h
    Meshes.cpp
    Hud.h
    Hud.cpp
    UndertimeModule.h
    UndertimeModule.cpp
    SceneComponents.h
    CavePreview.cpp
)

if(CHERRY_MONOLITHIC)
    add_library(UndertimeGame STATIC ${UndertimeSrc})
else()
    add_library(UndertimeGame SHARED ${UndertimeSrc})
endif()

target_link_libraries(UndertimeGame PRIVATE CherryEngine)
cherry_register_game_module(UndertimeGame)

if(MSVC)
    target_compile_options(UndertimeGame PRIVATE /W3 /utf-8)
endif()

# Pure voxel/physics helper tests: ctest -C Debug
add_executable(UndertimeTests Tests/UndertimeTests.cpp VoxelWorld.cpp Fragments.cpp Structure.cpp CaveGenerator.cpp)
target_link_libraries(UndertimeTests PRIVATE CherryEngine)
add_test(NAME UndertimeTests COMMAND UndertimeTests)
