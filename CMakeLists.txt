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

option(CHERRY_MONOLITHIC "Build as a single monolithic executable" OFF)
set(CHERRY_ACTIVE_GAME_TARGET "RubikGame" CACHE STRING "Game target used by launcher")

# Engine linkage mode is derived from build mode.
if(CHERRY_MONOLITHIC)
  set(CHERRYENGINE_BUILD_SHARED OFF CACHE BOOL "Build CherryEngine as shared library (DLL)" FORCE)
else()
  set(CHERRYENGINE_BUILD_SHARED ON CACHE BOOL "Build CherryEngine as shared library (DLL)" FORCE)
endif()

function(cherry_copy_runtime_asset_bundle target_name)
  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:${target_name}> $<TARGET_RUNTIME_DLLS:${target_name}>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/CherryEngine/Shaders"
      $<TARGET_FILE_DIR:${target_name}>/Shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/CherryEngine/Textures"
      $<TARGET_FILE_DIR:${target_name}>/Textures
    COMMAND_EXPAND_LISTS
  )

  if(DEFINED CHERRY_DXIL_DLL_PATH AND EXISTS "${CHERRY_DXIL_DLL_PATH}")
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CHERRY_DXIL_DLL_PATH}"
        $<TARGET_FILE_DIR:${target_name}>
    )
  endif()
endfunction()

function(cherry_copy_target_runtime target_name source_target)
  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:${source_target}>
      $<TARGET_FILE_DIR:${target_name}>
  )
endfunction()

add_subdirectory(CherryEngine)
add_subdirectory(Rubik)
add_subdirectory(CherryLaunch)
