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

# Option to build the engine as shared (DLL) or static library
option(CHERRYENGINE_BUILD_SHARED "Build CherryEngine as shared library (DLL)" OFF)

add_subdirectory(CherryEngine)
add_subdirectory(Rubik)
