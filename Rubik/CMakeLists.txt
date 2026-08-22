cmake_minimum_required(VERSION 3.22.1)

project(Rubik LANGUAGES CXX)

set(RubikSrc
    Rubik.cpp
    Rubik.h
    RubikManager.h
    RubikManager.cpp
)

add_executable(${PROJECT_NAME} ${RubikSrc})

# Link against CherryEngine target to inherit includes and compile definitions
target_link_libraries(${PROJECT_NAME} PRIVATE CherryEngine)

# Ensure CherryEngine runtime dependencies and assets are available next to the executable
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:CherryEngine> $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/CherryEngine/thirdparty/dxc/bin/x64/dxcompiler.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/CherryEngine/thirdparty/dxc/bin/x64/dxil.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/CherryEngine/Shaders"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/Shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/CherryEngine/Textures"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/Textures
)
