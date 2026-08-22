cmake_minimum_required(VERSION 3.22.1)

project(Rubik LANGUAGES CXX)

set(RubikSrc
    Rubik.cpp
    Rubik.h
    RubikModule.cpp
    RubikManager.h
    RubikManager.cpp
)

if(CHERRY_MONOLITHIC)
    add_library(RubikGame STATIC ${RubikSrc})
else()
    add_library(RubikGame SHARED ${RubikSrc})
endif()

# Link against CherryEngine target to inherit includes and compile definitions
target_link_libraries(RubikGame PRIVATE CherryEngine)
