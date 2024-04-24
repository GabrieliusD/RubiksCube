cmake_minimum_required(VERSION 3.10)

project(RubicCube VERSION 1.0)

set(HEADERS 
	Common.h
	Cube.h
	D3D12Resources.h
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
)

set(SOURCES
	Source.cpp
	Common.cpp
	Cube.cpp
	D3D12Resources.cpp
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
)


add_subdirectory(thirdparty/dxr/nv_helpers_dx12)

add_subdirectory(thirdparty/imgui)

add_subdirectory(thirdparty/openxr)

add_executable(RubicCube ${SOURCES} ${HEADERS})

target_link_directories(RubicCube PUBLIC "${PROJECT_SOURCE_DIR}/lib")

target_link_libraries(RubicCube PUBLIC openxr PUBLIC imgui PUBLIC dxr PUBLIC openxr_loaderd)

target_compile_definitions (RubicCube PUBLIC -D_UNICODE -DUNICODE)

target_include_directories(RubicCube PUBLIC "${PROJECT_SOURCE_DIR}" 
"${PROJECT_SOURCE_DIR}/thirdparty"
"${PROJECT_SOURCE_DIR}/thirdparty/dxr"
"${PROJECT_SOURCE_DIR}/thirdparty/dxr/nv_helpers_dx12"
"${PROJECT_SOURCE_DIR}/thirdparty/openxr"
"${PROJECT_SOURCE_DIR}/thirdparty/openxr/Common"
"${PROJECT_SOURCE_DIR}/thirdparty/imgui")

file(COPY Textures DESTINATION ${RubicCube_BINARY_DIR})


add_custom_command(TARGET RubicCube POST_BUILD        
    COMMAND ${CMAKE_COMMAND} -E copy_if_different  
        "${PROJECT_SOURCE_DIR}/lib/dxcompiler.dll"      
        $<TARGET_FILE_DIR:RubicCube>
	COMMAND ${CMAKE_COMMAND} -E copy_if_different  
        "${PROJECT_SOURCE_DIR}/lib/dxil.dll"      
        $<TARGET_FILE_DIR:RubicCube>
	COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/Shaders"
        $<TARGET_FILE_DIR:RubicCube>/Shaders
)                 
