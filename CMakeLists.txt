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

# Game modules call this so the launcher builds them. Each game is a project: a
# <Name>.cherryproject file next to its CMakeLists.txt, with its Content/ beside it.
# The editor reads Content/ straight from the source tree; only the monolithic build
# copies the active game's Content/ next to the executable.
function(cherry_register_game_module target_name)
  set_property(GLOBAL APPEND PROPERTY CHERRY_GAME_MODULES ${target_name})
  if(MSVC AND NOT CHERRY_MONOLITHIC)
    # Embed only the PDB's file name: the editor loads a copy of the DLL with a copy of
    # its PDB, and the debugger then finds that copy instead of locking the original.
    # (Not %_PDB%: the Visual Studio generator escapes the percent signs.)
    target_link_options(${target_name} PRIVATE "/PDBALTPATH:${target_name}.pdb")
  endif()
endfunction()

# Copies what `target_name` needs at runtime into its folder: engine shaders, the
# DirectX Shader Compiler DLLs and (monolithic builds) the active game's Content/. This runs on
# every build (not as a post-link step), so asset edits are picked up even when
# nothing relinks.
function(cherry_add_runtime_assets target_name)
  set(out_dir "$<TARGET_FILE_DIR:${target_name}>")
  set(commands
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/CherryEngine/Shaders" "${out_dir}/Shaders")
  if(TARGET Jolt::Jolt)
    get_target_property(_jolt_type Jolt::Jolt TYPE)
    if(_jolt_type STREQUAL "SHARED_LIBRARY")
      list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Jolt::Jolt>" "${out_dir}")
    endif()
  endif()
  foreach(dll IN ITEMS "${CHERRY_DXC_DLL_PATH}" "${CHERRY_DXIL_DLL_PATH}")
    if(dll AND EXISTS "${dll}")
      list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${out_dir}")
    endif()
  endforeach()
  if(CHERRY_MONOLITHIC AND TARGET ${CHERRY_ACTIVE_GAME_TARGET})
    get_target_property(game_source_dir ${CHERRY_ACTIVE_GAME_TARGET} SOURCE_DIR)
    if(EXISTS "${game_source_dir}/Content")
      list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_directory "${game_source_dir}/Content" "${out_dir}/Content")
    endif()
    # The project file names the startup scene.
    file(GLOB _project_files "${game_source_dir}/*.cherryproject")
    foreach(_project_file IN LISTS _project_files)
      list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_project_file}" "${out_dir}")
    endforeach()
  endif()

  add_custom_target(${target_name}RuntimeAssets ALL ${commands}
    COMMENT "Copying runtime assets for ${target_name}" VERBATIM)
  add_dependencies(${target_name} ${target_name}RuntimeAssets)
endfunction()

enable_testing()

# Jolt Physics (vcpkg: joltphysics), used privately by the engine. Found here so the
# launcher can copy its DLL next to the executable.
find_package(Jolt CONFIG REQUIRED)

add_subdirectory(CherryEngine)
add_subdirectory(Rubik)
add_subdirectory(Undertime)
add_subdirectory(CherryLaunch)
