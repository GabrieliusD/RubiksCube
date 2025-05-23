set(HEADERS
	BottomLevelASGenerator.h
	DXRHelper.h
	RaytracingPipelineGenerator.h
	RootSignatureGenerator.h
	ShaderBindingTableGenerator.h
	TopLevelASGenerator.h
)

set(SOURCES
	BottomLevelASGenerator.cpp
	RaytracingPipelineGenerator.cpp
	RootSignatureGenerator.cpp
	ShaderBindingTableGenerator.cpp
	TopLevelASGenerator.cpp
)

add_library(dxr ${HEADERS} ${SOURCES})


install(TARGETS dxr DESTINATION lib)
install(FILES ${HEADERS} ${SOURCES} DESTINATION include)