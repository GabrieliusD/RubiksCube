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
    LooseBlocks.h
    LooseBlocks.cpp
    Structure.h
    Structure.cpp
    Noise.h
    CaveGenerator.h
    CaveGenerator.cpp
    CaveMap.h
    CaveMap.cpp
    Valley.h
    Valley.cpp
    TerrainMesh.h
    TerrainMesh.cpp
    ValleyScenery.h
    ValleyScenery.cpp
    ValleyProps.h
    ValleyProps.cpp
    ValleyPreview.cpp
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
add_executable(UndertimeTests Tests/UndertimeTests.cpp Tests/ValleyTests.cpp VoxelWorld.cpp Fragments.cpp LooseBlocks.cpp Meshes.cpp Structure.cpp CaveGenerator.cpp
    Valley.cpp TerrainMesh.cpp)
target_link_libraries(UndertimeTests PRIVATE CherryEngine)
add_test(NAME UndertimeTests COMMAND UndertimeTests)

# Writes the placeholder voxel models (Content/Models/Placeholders/*.vox). Run by hand:
#   bin/<Config>/UndertimePlaceholders.exe
add_executable(UndertimePlaceholders Tools/Placeholders.cpp)
target_link_libraries(UndertimePlaceholders PRIVATE CherryEngine)
target_compile_definitions(UndertimePlaceholders PRIVATE UNDERTIME_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Content")
