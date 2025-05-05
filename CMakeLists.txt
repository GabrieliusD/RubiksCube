cmake_minimum_required(VERSION 3.22.1)

project(GraphicsProject LANGUAGES CSharp CXX)


set(CMAKE_CONFIGURATION_TYPES "Debug;Release")

if(MSVC)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++17")
endif(MSVC)


add_subdirectory(RubicCube)
add_subdirectory(SampleProject)