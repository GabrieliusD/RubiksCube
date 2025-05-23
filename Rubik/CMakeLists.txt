cmake_minimum_required(VERSION 3.30.5)

project(Rubik LANGUAGES CXX)

set(RubikSrc "Rubik.cpp" "Rubik.h")

add_executable(${PROJECT_NAME} ${RubikSrc})


target_link_libraries(${PROJECT_NAME} CherryEngine)

add_custom_command ( TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:CherryEngine> $<TARGET_FILE_DIR:${PROJECT_NAME}>
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD        
    COMMAND ${CMAKE_COMMAND} -E copy_if_different  
        "${CMAKE_SOURCE_DIR}/CherryEngine/lib/dxcompiler.dll"      
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
	COMMAND ${CMAKE_COMMAND} -E copy_if_different  
        "${CMAKE_SOURCE_DIR}/CherryEngine/lib/dxil.dll"      
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
	COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/CherryEngine/Shaders"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/Shaders
	COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/CherryEngine/Textures"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/Textures
)  
