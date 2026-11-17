cmake_minimum_required(VERSION 3.22.1)

project(CherryEngine VERSION 1.0 LANGUAGES CXX)

# Allow top-level option to control shared/static build
if(NOT DEFINED CHERRYENGINE_BUILD_SHARED)
  option(CHERRYENGINE_BUILD_SHARED "Build CherryEngine as shared library (DLL)" ON)
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
	ShaderSystem/ShaderDesc.h
	ShaderSystem/ShaderCompiler.h
 "Entity.h" "RenderSystem.h" "Transform.h" "Graphics/DescriptorHeap.h" "Graphics/D3DCore.h" "Camera.h" "CameraSystem.h")

set(SOURCES
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
	Graphics/RenderTexture.cpp
	UserInterface/ViewportWindow.cpp
	ShaderSystem/ShaderCompiler.cpp
 "Entity.h" "RenderSystem.h" "RenderSystem.cpp" "Graphics/DescriptorHeap.cpp" "Graphics/D3DCore.cpp" "CameraSystem.cpp")

add_subdirectory(thirdparty/dxr/nv_helpers_dx12)
add_subdirectory(thirdparty/imgui)

find_package(directx-dxc CONFIG QUIET)

set(CHERRY_DXC_TARGET "")
foreach(candidate_target IN ITEMS Microsoft::DirectXShaderCompiler directx-dxc::dxcompiler unofficial::directx-dxc::dxcompiler DXC::dxcompiler)
  if(TARGET ${candidate_target})
    set(CHERRY_DXC_TARGET ${candidate_target})
    break()
  endif()
endforeach()

if(CHERRY_DXC_TARGET STREQUAL "")
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_vcpkg_triplet_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    set(_dxc_inc "${_vcpkg_triplet_dir}/include")
    set(_dxc_lib_release "${_vcpkg_triplet_dir}/lib/dxcompiler.lib")
    set(_dxc_dll_release "${_vcpkg_triplet_dir}/bin/dxcompiler.dll")
    set(_dxc_lib_debug "${_vcpkg_triplet_dir}/debug/lib/dxcompiler.lib")
    set(_dxc_dll_debug "${_vcpkg_triplet_dir}/debug/bin/dxcompiler.dll")

    if(EXISTS "${_dxc_lib_release}" AND EXISTS "${_dxc_dll_release}")
      add_library(DXC::dxcompiler SHARED IMPORTED GLOBAL)
      set_target_properties(DXC::dxcompiler PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_dxc_inc}"
        IMPORTED_IMPLIB_RELEASE "${_dxc_lib_release}"
        IMPORTED_LOCATION_RELEASE "${_dxc_dll_release}"
      )

      if(EXISTS "${_dxc_lib_debug}" AND EXISTS "${_dxc_dll_debug}")
        set_target_properties(DXC::dxcompiler PROPERTIES
          IMPORTED_IMPLIB_DEBUG "${_dxc_lib_debug}"
          IMPORTED_LOCATION_DEBUG "${_dxc_dll_debug}"
        )
      endif()

      set(CHERRY_DXC_TARGET DXC::dxcompiler)
    endif()
  endif()
endif()

if(CHERRY_DXC_TARGET STREQUAL "")
  message(FATAL_ERROR "Could not find DirectX Shader Compiler (DXC) library. Please install it via vcpkg or ensure it is available on your system.")
endif()

set(CHERRY_DXC_DLL_PATH "")
foreach(cfg IN ITEMS RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
  get_target_property(_dxc_dll_candidate ${CHERRY_DXC_TARGET} IMPORTED_LOCATION_${cfg})
  if(_dxc_dll_candidate)
    set(CHERRY_DXC_DLL_PATH "${_dxc_dll_candidate}")
    break()
  endif()
endforeach()
if(CHERRY_DXC_DLL_PATH STREQUAL "")
  get_target_property(CHERRY_DXC_DLL_PATH ${CHERRY_DXC_TARGET} IMPORTED_LOCATION)
endif()

if(CHERRY_DXC_DLL_PATH)
  get_filename_component(_dxc_bin_dir "${CHERRY_DXC_DLL_PATH}" DIRECTORY)
  set(CHERRY_DXIL_DLL_PATH "${_dxc_bin_dir}/dxil.dll")
  set(CHERRY_DXIL_DLL_PATH "${CHERRY_DXIL_DLL_PATH}" CACHE INTERNAL "Path to dxil.dll sidecar for DXC" FORCE)
endif()

if(CHERRYENGINE_BUILD_SHARED)
  add_library(CherryEngine SHARED ${SOURCES} ${HEADERS})
  target_compile_definitions(CherryEngine PRIVATE CHERRYENGINE_EXPORTS)
  set_target_properties(CherryEngine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
  add_library(CherryEngine STATIC ${SOURCES} ${HEADERS})
endif()

# Imported OpenXR helper
add_library(openxr_loaderd SHARED IMPORTED GLOBAL)
set_property(TARGET openxr_loaderd PROPERTY IMPORTED_IMPLIB "${PROJECT_SOURCE_DIR}/lib/openxr_loaderd.lib")

# Link dependencies
target_link_libraries(CherryEngine PRIVATE imgui dxr ${CHERRY_DXC_TARGET} openxr_loaderd)

target_compile_definitions(CherryEngine PUBLIC _UNICODE UNICODE)

target_include_directories(CherryEngine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui>
)

cherry_copy_runtime_asset_bundle(CherryEngine)

include(GNUInstallDirs)

install(TARGETS CherryEngine
  EXPORT CherryEngineTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

foreach(header IN LISTS HEADERS)
  get_filename_component(header_dir "${header}" DIRECTORY)
  if(header_dir STREQUAL "")
    set(header_install_dir "${CMAKE_INSTALL_INCLUDEDIR}/CherryEngine")
  else()
    set(header_install_dir "${CMAKE_INSTALL_INCLUDEDIR}/CherryEngine/${header_dir}")
  endif()

  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${header}" DESTINATION "${header_install_dir}")
endforeach()

install(EXPORT CherryEngineTargets
  FILE CherryEngineTargets.cmake
  NAMESPACE Cherry::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CherryEngine
)

if(CHERRY_DXC_DLL_PATH)
  install(FILES "${CHERRY_DXC_DLL_PATH}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(DEFINED CHERRY_DXIL_DLL_PATH AND EXISTS "${CHERRY_DXIL_DLL_PATH}")
  install(FILES "${CHERRY_DXIL_DLL_PATH}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
