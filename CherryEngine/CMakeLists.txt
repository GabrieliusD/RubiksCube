cmake_minimum_required(VERSION 3.10)

project(CherryEngine VERSION 1.0)

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
	openxr/MathLib.h
	openxr/OpenXrManager.h
 "Entity.h" "RenderSystem.h" "Transform.h" "Graphics/DescriptorHeap.h" "Graphics/D3DCore.h" "Camera.h" "CameraSystem.h" "Graphics/RenderTexture.h" "UserInterface/ViewportWindow.h")

set(SOURCES
	"EntryPoint.h"
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
	openxr/MathLib.cpp
	openxr/OpenXrManager.cpp
 "Entity.h" "RenderSystem.h" "RenderSystem.cpp" "Graphics/DescriptorHeap.cpp" "Graphics/D3DCore.cpp" "CameraSystem.cpp" "Graphics/RenderTexture.cpp"
 "UserInterface/ViewportWindow.cpp")


add_subdirectory(thirdparty/dxr/nv_helpers_dx12)

add_subdirectory(thirdparty/imgui)

add_subdirectory(thirdparty/openxr)

add_library(CherryEngine STATIC ${SOURCES} ${HEADERS})

add_library(dxcompiler SHARED IMPORTED GLOBAL)
set_property(TARGET dxcompiler PROPERTY IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/dxcompiler.dll")
set_property(TARGET dxcompiler PROPERTY IMPORTED_IMPLIB "${PROJECT_SOURCE_DIR}/lib/openxr_loaderd.lib")

target_link_directories(CherryEngine PUBLIC "${PROJECT_SOURCE_DIR}/lib")

target_link_libraries(CherryEngine PUBLIC openxr PUBLIC imgui PUBLIC dxr PUBLIC openxr_loaderd PUBLIC dxcompiler)
target_compile_definitions (CherryEngine PUBLIC -D_UNICODE -DUNICODE)

target_include_directories(CherryEngine PUBLIC "${PROJECT_SOURCE_DIR}" 
"${PROJECT_SOURCE_DIR}/thirdparty"
"${PROJECT_SOURCE_DIR}/thirdparty/dxr"
"${PROJECT_SOURCE_DIR}/thirdparty/dxr/nv_helpers_dx12"
"${PROJECT_SOURCE_DIR}/thirdparty/openxr"
"${PROJECT_SOURCE_DIR}/thirdparty/openxr/Common"
"${PROJECT_SOURCE_DIR}/thirdparty/imgui"
"${CMAKE_CURRENT_SOURCE_DIR}")

#file(COPY Textures DESTINATION ${RubicCube_BINARY_DIR})
#file(COPY Shaders DESTINATION ${RubicCube_BINARY_DIR})               

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