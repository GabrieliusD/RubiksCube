cmake_minimum_required(VERSION 3.30.5)

project(GraphicsProject LANGUAGES CXX)


set(CMAKE_CONFIGURATION_TYPES "Debug;Release")

if(MSVC)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++17")
endif(MSVC)


add_subdirectory(CherryEngine)
add_subdirectory(Rubik)