cmake_minimum_required(VERSION 3.22.1)

project(GraphicsProject LANGUAGES CXX)

# Use modern CMake settings for C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Multi-config generators (Visual Studio) can use these configuration types
if(POLICY CMP0063)
  cmake_policy(SET CMP0063 NEW)
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported configuration types" FORCE)

option(CHERRY_MONOLITHIC "Build as a single monolithic executable" OFF)
set(CHERRY_ACTIVE_GAME_TARGET "RubikGame" CACHE STRING "Game target used by launcher")

# Engine linkage mode is derived from build mode.
if(CHERRY_MONOLITHIC)
  set(CHERRYENGINE_BUILD_SHARED OFF CACHE BOOL "Build CherryEngine as shared library (DLL)" FORCE)
else()
  set(CHERRYENGINE_BUILD_SHARED ON CACHE BOOL "Build CherryEngine as shared library (DLL)" FORCE)
endif()

add_subdirectory(CherryEngine)
add_subdirectory(Rubik)
add_subdirectory(CherryLaunch)
