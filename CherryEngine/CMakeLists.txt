cmake_minimum_required(VERSION 3.22.1)

project(CherryEngine VERSION 1.0 LANGUAGES CXX)

# Allow top-level option to control shared/static build
if(NOT DEFINED CHERRYENGINE_BUILD_SHARED)
  option(CHERRYENGINE_BUILD_SHARED "Build CherryEngine as shared library (DLL)" ON)
endif()

set(ENGINE_HEADERS
  CherryAPI.h
  Core/Engine.h
  Core/Project.h
  Core/ModuleHost.h
  Core/BuildProcess.h
  Core/Assets.h
  Core/AssetImport.h
  Core/Input.h
  Core/Log.h
  Core/Window.h
  Core/CameraControllers.h
  Game/GameModule.h
  Game/Script.h
  Game/BuiltinScripts.h
  Scene/ECS.h
  Scene/Components.h
  Scene/Scene.h
  Scene/Reflection.h
  Scene/SceneSerializer.h
  Renderer/Renderer.h
  Renderer/RenderTypes.h
  Renderer/ResourcePool.h
  Renderer/DebugDraw.h
  Editor/EditorLayer.h
  Editor/Gizmo.h
  Editor/ShaderGraphEditor.h
  Physics/PhysicsWorld.h
  Voxel/VoxelModel.h
  RHI/RHI.h
  RHI/Pipeline.h
  RHI/D3D12/D3DCommon.h
  RHI/D3D12/DescriptorHeap.h
  Renderer/ImGuiRenderer.h
  Renderer/DdsReader.h
  ShaderSystem/ShaderDesc.h
  ShaderSystem/ShaderCompiler.h
  ShaderGraph/ShaderGraph.h
  ShaderGraph/NodeLibrary.h
  ShaderGraph/GraphCompiler.h
  Shaders/Shared/ShaderInterop.h
  DDSTextureLoader.h
  GeometryGenerator.h
  MathHelper.h
  d3dx12.h
)

set(ENGINE_SOURCES
  Core/Engine.cpp
  Core/Project.cpp
  Core/ModuleHost.cpp
  Core/BuildProcess.cpp
  Core/Assets.cpp
  Core/AssetImport.cpp
  Core/Input.cpp
  Core/Log.cpp
  Core/Window.cpp
  Scene/Scene.cpp
  Game/Script.cpp
  Game/BuiltinScripts.cpp
  Scene/Reflection.cpp
  Scene/SceneSerializer.cpp
  Renderer/Renderer.cpp
  Renderer/ImGuiRenderer.cpp
  Renderer/DdsReader.cpp
  Renderer/DebugDraw.cpp
  Editor/EditorLayer.cpp
  Editor/ScenePanels.cpp
  Editor/Gizmo.cpp
  Editor/ContentBrowser.cpp
  Editor/ShaderGraphEditor.cpp
  Physics/PhysicsWorld.cpp
  Voxel/VoxelModel.cpp
  Particles/ParticleSystem.cpp
  RHI/RHI.cpp
  RHI/Pipeline.cpp
  RHI/D3D12/D3DCommon.cpp
  RHI/D3D12/DescriptorHeap.cpp
  RHI/D3D12/D3D12Device.cpp
  ShaderSystem/ShaderCompiler.cpp
  ShaderGraph/ShaderGraph.cpp
  ShaderGraph/NodeLibrary.cpp
  ShaderGraph/GraphCompiler.cpp
  GeometryGenerator.cpp
  MathHelper.cpp
)

# The editor (scene editing, hot-reloaded game modules, the shader graph editor) and the
# D3D12 renderer are Windows-only. Other platforms (Android) build a packaged game: the
# engine without them, the game linked in (CHERRY_MONOLITHIC), and a renderer without a
# graphics backend until the Vulkan one (Dice/Docs/Android.md).
if(WIN32)
  set(CHERRY_EDITOR ON)
else()
  set(CHERRY_EDITOR OFF)
endif()
set(CHERRY_EDITOR ${CHERRY_EDITOR} PARENT_SCOPE)

if(NOT WIN32)
  set(_cherry_windows_only
    Core/ModuleHost.h Core/ModuleHost.cpp Core/BuildProcess.h Core/BuildProcess.cpp
    Core/Window.cpp
    Renderer/Renderer.cpp Renderer/ImGuiRenderer.h Renderer/ImGuiRenderer.cpp Renderer/DdsReader.h Renderer/DdsReader.cpp
    Editor/EditorLayer.h Editor/EditorLayer.cpp Editor/ScenePanels.cpp Editor/Gizmo.h Editor/Gizmo.cpp
    Editor/ContentBrowser.cpp Editor/ShaderGraphEditor.h Editor/ShaderGraphEditor.cpp
    RHI/RHI.h RHI/RHI.cpp RHI/Pipeline.h RHI/Pipeline.cpp
    RHI/D3D12/D3DCommon.h RHI/D3D12/DescriptorHeap.h RHI/D3D12/D3DCommon.cpp RHI/D3D12/DescriptorHeap.cpp RHI/D3D12/D3D12Device.cpp
    ShaderSystem/ShaderCompiler.h ShaderSystem/ShaderCompiler.cpp
    DDSTextureLoader.h d3dx12.h)
  list(REMOVE_ITEM ENGINE_HEADERS ${_cherry_windows_only})
  list(REMOVE_ITEM ENGINE_SOURCES ${_cherry_windows_only})
  list(APPEND ENGINE_HEADERS Core/NoEditor.h)
  if(ANDROID)
    # The mobile renderer: Vulkan, the same Renderer API as the D3D12 one, with the passes
    # the games on Android need (Dice/Docs/Android.md).
    list(APPEND ENGINE_SOURCES Platform/Android/Window_Android.cpp Renderer/Vulkan/VulkanRenderer.cpp)
  else()
    list(APPEND ENGINE_SOURCES Renderer/NullRenderer.cpp)
  endif()
endif()

# Shaders are compiled at runtime by DXC; listed here so they show up in the IDE.
file(GLOB ENGINE_SHADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl"
  "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsli")
set_source_files_properties(${ENGINE_SHADERS} PROPERTIES HEADER_FILE_ONLY TRUE)

# Dear ImGui is compiled into the engine and exported from it, so game modules
# share the engine's ImGui context instead of getting their own copy.
set(IMGUI_DIR thirdparty/imgui)
set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
)
if(WIN32)
  list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/imgui_impl_win32.cpp)
endif()
# imgui-node-editor (the shader graph editor), compiled into the engine with ImGui.
set(NODE_EDITOR_DIR thirdparty/imgui-node-editor)
set(NODE_EDITOR_SOURCES
  ${NODE_EDITOR_DIR}/crude_json.cpp
  ${NODE_EDITOR_DIR}/imgui_canvas.cpp
  ${NODE_EDITOR_DIR}/imgui_node_editor.cpp
  ${NODE_EDITOR_DIR}/imgui_node_editor_api.cpp
)
if(CHERRY_EDITOR)
  if(MSVC)
    set_source_files_properties(${NODE_EDITOR_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")
  endif()
  list(APPEND IMGUI_SOURCES ${NODE_EDITOR_SOURCES})
endif()

# ---------------------------------------------------------------------------
# DirectX Shader Compiler (vcpkg: directx-dxc)
# ---------------------------------------------------------------------------
if(WIN32)
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
  set(CHERRY_DXIL_DLL_PATH "${_dxc_bin_dir}/dxil.dll" CACHE INTERNAL "Path to dxil.dll sidecar for DXC" FORCE)
  set(CHERRY_DXC_DLL_PATH "${CHERRY_DXC_DLL_PATH}" CACHE INTERNAL "Path to dxcompiler.dll" FORCE)
endif()
endif() # WIN32

# ---------------------------------------------------------------------------
# Mobile shaders (Android): HLSL in Shaders/Mobile compiled to SPIR-V at build time by the
# host's DXC (vcpkg host dependency) into headers the Vulkan renderer embeds.
# ---------------------------------------------------------------------------
function(cherry_mobile_shaders target)
  set(_host_triplet "${VCPKG_HOST_TRIPLET}")
  if(NOT _host_triplet)
    set(_host_triplet x64-windows)
  endif()
  set(_dxc_dir "${VCPKG_INSTALLED_DIR}/${_host_triplet}/tools/directx-dxc")
  find_program(CHERRY_HOST_DXC NAMES dxc dxc.exe PATHS "${_dxc_dir}" NO_DEFAULT_PATH)
  if(NOT CHERRY_HOST_DXC)
    message(FATAL_ERROR "No host DXC in ${_dxc_dir} (vcpkg.json: directx-dxc as a host dependency)")
  endif()
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Mobile")
  set(_out "${CMAKE_CURRENT_BINARY_DIR}/MobileShaders")
  file(GLOB _includes CONFIGURE_DEPENDS "${_src}/*.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Shared/*.h")
  # name  file  entry  profile
  set(_shaders
    ImGuiVS ImGui.hlsl VSMain vs_6_0
    ImGuiPS ImGui.hlsl PSMain ps_6_0
    MeshVS Mesh.hlsl VSMain vs_6_0
    MeshPS Mesh.hlsl PSMain ps_6_0
    ShadowVS Shadow.hlsl VSMain vs_6_0
    ParticlesVS Particles.hlsl VSMain vs_6_0
    ParticlesPS Particles.hlsl PSMain ps_6_0
    PostVS Post.hlsl VSMain vs_6_0
    TonemapPS Post.hlsl TonemapPS ps_6_0
    CopyPS Post.hlsl CopyPS ps_6_0
    FxaaPS Post.hlsl FxaaPS ps_6_0)
  set(_headers "")
  list(LENGTH _shaders _count)
  math(EXPR _last "${_count} - 1")
  foreach(_i RANGE 0 ${_last} 4)
    math(EXPR _f "${_i} + 1")
    math(EXPR _e "${_i} + 2")
    math(EXPR _p "${_i} + 3")
    list(GET _shaders ${_i} _name)
    list(GET _shaders ${_f} _file)
    list(GET _shaders ${_e} _entry)
    list(GET _shaders ${_p} _profile)
    set(_header "${_out}/${_name}.spv.h")
    # Scene and post vertex shaders keep D3D's clip space (y up), like the engine's shaders;
    # ImGui's works in Vulkan's directly.
    set(_flip "")
    if(_profile MATCHES "^vs_" AND NOT _file STREQUAL "ImGui.hlsl")
      set(_flip -fvk-invert-y)
    endif()
    add_custom_command(OUTPUT "${_header}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out}"
      COMMAND "${CHERRY_HOST_DXC}" -spirv -fspv-target-env=vulkan1.1 ${_flip} -T ${_profile} -E ${_entry}
        -I "${_src}" -I "${CMAKE_CURRENT_SOURCE_DIR}/Shaders" -Fh "${_header}" -Vn kSpirv_${_name} "${_src}/${_file}"
      DEPENDS "${_src}/${_file}" ${_includes}
      COMMENT "SPIR-V ${_name}" VERBATIM)
    list(APPEND _headers "${_header}")
  endforeach()
  add_custom_target(${target}MobileShaders DEPENDS ${_headers})
  add_dependencies(${target} ${target}MobileShaders)
  target_include_directories(${target} PRIVATE "${_out}")
endfunction()

# ---------------------------------------------------------------------------
# Engine target
# ---------------------------------------------------------------------------
if(CHERRYENGINE_BUILD_SHARED)
  add_library(CherryEngine SHARED ${ENGINE_SOURCES} ${ENGINE_HEADERS} ${IMGUI_SOURCES} ${ENGINE_SHADERS})
  if(WIN32)
    target_compile_definitions(CherryEngine
      PRIVATE CHERRYENGINE_EXPORTS "IMGUI_API=__declspec(dllexport)"
      INTERFACE CHERRYENGINE_SHARED "IMGUI_API=__declspec(dllimport)")
  endif()
else()
  add_library(CherryEngine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS} ${IMGUI_SOURCES} ${ENGINE_SHADERS})
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS} ${ENGINE_SHADERS})
source_group("ThirdParty\\imgui" FILES ${IMGUI_SOURCES})

# Jolt Physics stays private to the engine: games use PhysicsWorld's handle-based API.
find_package(nlohmann_json CONFIG REQUIRED)
# Header-only asset libraries (vcpkg: cgltf, stb): glTF meshes and PNG/JPG/TGA textures.
find_path(CHERRY_CGLTF_INCLUDE_DIR "cgltf.h" REQUIRED)
find_path(CHERRY_STB_INCLUDE_DIR "stb_image.h" REQUIRED)
target_include_directories(CherryEngine PRIVATE "${CHERRY_CGLTF_INCLUDE_DIR}" "${CHERRY_STB_INCLUDE_DIR}")
target_include_directories(CherryEngine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${NODE_EDITOR_DIR}")
if(WIN32)
  target_link_libraries(CherryEngine PRIVATE ${CHERRY_DXC_TARGET} d3d12 dxgi dxguid comdlg32 Jolt::Jolt)
else()
  target_link_libraries(CherryEngine PRIVATE Jolt::Jolt)
  # DirectXMath (header-only) is part of the Windows SDK; elsewhere it comes from vcpkg.
  find_package(directxmath CONFIG REQUIRED)
  target_link_libraries(CherryEngine PUBLIC Microsoft::DirectXMath)
  if(ANDROID)
    target_link_libraries(CherryEngine PUBLIC log android vulkan)
    cherry_mobile_shaders(CherryEngine)
  endif()
endif()
target_compile_definitions(CherryEngine PUBLIC CHERRY_EDITOR=$<BOOL:${CHERRY_EDITOR}>)
# nlohmann/json (header-only) is public: game modules read and write JSON with it too
# (UNDERTIME's dialog files and save game).
target_link_libraries(CherryEngine PUBLIC nlohmann_json::nlohmann_json)

target_compile_definitions(CherryEngine
  PUBLIC _UNICODE UNICODE NOMINMAX
  # Debug builds compile shaders straight from the source tree so edits hot-reload.
  PRIVATE CHERRY_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
  # The editor finds projects in the source tree and rebuilds game modules with this
  # CMake, build folder and configuration.
  PRIVATE CHERRY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
          CHERRY_CMAKE_COMMAND="${CMAKE_COMMAND}"
          CHERRY_BUILD_DIR="${CMAKE_BINARY_DIR}"
          CHERRY_BUILD_CONFIG="$<CONFIG>")

if(MSVC)
  target_compile_options(CherryEngine PRIVATE /MP /W3 /utf-8)
endif()

target_include_directories(CherryEngine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${IMGUI_DIR}>
    $<INSTALL_INTERFACE:include>
)



# ---------------------------------------------------------------------------
# Tests: scene, reflection and serialization (no window or GPU needed)
# ---------------------------------------------------------------------------
if(WIN32)
add_executable(CherryEngineTests Tests/EngineTests.cpp Tests/ShaderGraphTests.cpp Tests/VoxelModelTests.cpp Tests/RendererTests.cpp)
target_link_libraries(CherryEngineTests PRIVATE CherryEngine)
# Generated shader graph code is compiled with DXC against the engine's shaders.
target_compile_definitions(CherryEngineTests PRIVATE CHERRY_TEST_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
  CHERRY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data")
if(MSVC)
  target_compile_options(CherryEngineTests PRIVATE /W3 /utf-8)
endif()
add_test(NAME CherryEngineTests COMMAND CherryEngineTests)
endif()

# ---------------------------------------------------------------------------
# Install (engine SDK: headers + library + runtime DLLs)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS CherryEngine
  EXPORT CherryEngineTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

foreach(header IN LISTS ENGINE_HEADERS)
  get_filename_component(header_dir "${header}" DIRECTORY)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${header_dir}")
endforeach()
install(FILES ${IMGUI_DIR}/imgui.h ${IMGUI_DIR}/imconfig.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY Shaders/ DESTINATION ${CMAKE_INSTALL_BINDIR}/Shaders)

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
