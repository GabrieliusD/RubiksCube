cmake_minimum_required(VERSION 3.10)

project(CherryEngine VERSION 1.0 LANGUAGES CXX)

# Allow top-level option to control shared/static build
if(NOT DEFINED CHERRYENGINE_BUILD_SHARED)
  option(CHERRYENGINE_BUILD_SHARED "Build CherryEngine as shared library (DLL)" OFF)
endif()

set(HEADERS 
    Common.h
    Cube.h
    D3DApp.h
    d3dUtil.h
    d3dx12.h
    DDSTextureLoader.h
    GameTimer.h
    Geometry.h
    GeometryGenerator.h
    MathHelper.h
    PassConstant.h
    RenderObject.h
    Graphics/Buffer.h
    Entity.h
    RenderSystem.h
    Transform.h
    Graphics/DescriptorHeap.h
    Graphics/D3DCore.h
    Camera.h
    CameraSystem.h
)

set(SOURCES
    EntryPoint.h
    Common.cpp
    Cube.cpp
    D3DApp.cpp
    d3dUtil.cpp
    DDSTextureLoader.cpp
    GameTimer.cpp
    Geometry.cpp
    GeometryGenerator.cpp
    MathHelper.cpp
    RenderObject.cpp
    RenderSystem.cpp
    Graphics/DescriptorHeap.cpp
    Graphics/D3DCore.cpp
    CameraSystem.cpp
)

add_subdirectory(thirdparty/dxr/nv_helpers_dx12)
add_subdirectory(thirdparty/imgui)

if(CHERRYENGINE_BUILD_SHARED)
  add_library(CherryEngine SHARED ${SOURCES} ${HEADERS})
  target_compile_definitions(CherryEngine PRIVATE CHERRYENGINE_EXPORTS)
else()
  add_library(CherryEngine STATIC ${SOURCES} ${HEADERS})
endif()

# Imported dxcompiler helper
add_library(dxcompiler SHARED IMPORTED GLOBAL)
set_property(TARGET dxcompiler PROPERTY IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/dxcompiler.dll")
set_property(TARGET dxcompiler PROPERTY IMPORTED_IMPLIB "${PROJECT_SOURCE_DIR}/lib/openxr_loaderd.lib")

# Link directories and libraries
target_link_directories(CherryEngine PUBLIC "${PROJECT_SOURCE_DIR}/lib")
target_link_libraries(CherryEngine PUBLIC imgui dxr openxr_loaderd dxcompiler)

target_compile_definitions(CherryEngine PUBLIC -D_UNICODE -DUNICODE)

target_include_directories(CherryEngine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui>
  $<INSTALL_INTERFACE:include>
)

# Install or copy runtime assets after build
add_custom_command(TARGET CherryEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/lib/dxcompiler.dll"
        $<TARGET_FILE_DIR:CherryEngine>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/lib/dxil.dll"
        $<TARGET_FILE_DIR:CherryEngine>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/Shaders"
        $<TARGET_FILE_DIR:CherryEngine>/Shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/Textures"
        $<TARGET_FILE_DIR:CherryEngine>/Textures
)
