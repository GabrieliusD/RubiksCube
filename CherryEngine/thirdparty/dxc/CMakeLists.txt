set(DXC_ROOT "${CMAKE_CURRENT_LIST_DIR}")

add_library(DXC::dxcompiler SHARED IMPORTED GLOBAL)

set_target_properties(DXC::dxcompiler PROPERTIES
    IMPORTED_IMPLIB "${DXC_ROOT}/lib/x64/dxcompiler.lib"
    IMPORTED_LOCATION "${DXC_ROOT}/bin/x64/dxcompiler.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${DXC_ROOT}/inc"
)

if(NOT EXISTS "${DXC_ROOT}/lib/x64/dxcompiler.lib")
    message(FATAL_ERROR "DXC import library not found: ${DXC_ROOT}/lib/x64/dxcompiler.lib")
endif()

if(NOT EXISTS "${DXC_ROOT}/bin/x64/dxcompiler.dll")
    message(FATAL_ERROR "DXC runtime not found: ${DXC_ROOT}/bin/x64/dxcompiler.dll")
endif()