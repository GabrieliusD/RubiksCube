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

if(MSVC)
  # Faster incremental builds, and one runtime for engine + game modules.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Every executable and DLL (launcher, engine, game modules, tests) goes into one
# folder per configuration, so the launcher always sees the freshly built DLLs.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Copies what `target_name` needs at runtime into its folder: engine shaders, the
# DirectX Shader Compiler DLLs and the game's Content/. This runs on every build
# (not as a post-link step), so asset edits are picked up even when nothing relinks.
function(cherry_add_runtime_assets target_name game_source_dir)
  set(out_dir "$<TARGET_FILE_DIR:${target_name}>")
  set(commands
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/CherryEngine/Shaders" "${out_dir}/Shaders")
  foreach(dll IN ITEMS "${CHERRY_DXC_DLL_PATH}" "${CHERRY_DXIL_DLL_PATH}")
    if(dll AND EXISTS "${dll}")
      list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${out_dir}")
    endif()
  endforeach()
  if(EXISTS "${game_source_dir}/Content")
    list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_directory "${game_source_dir}/Content" "${out_dir}/Content")
  endif()

  add_custom_target(${target_name}RuntimeAssets ALL ${commands}
    COMMENT "Copying runtime assets for ${target_name}" VERBATIM)
  add_dependencies(${target_name} ${target_name}RuntimeAssets)
endfunction()

enable_testing()

add_subdirectory(CherryEngine)
add_subdirectory(Rubik)
add_subdirectory(CherryLaunch)
