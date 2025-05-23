set(HEADERS
	"openxr.h"
	"openxr_loader_negotiation.h"
	"openxr_platform.h"
	"openxr_platform_defines.h"
	"openxr_reflection.h"
	"openxr_reflection_parent_structs.h"
	"openxr_reflection_structs.h"
	"OpenXRDebugUtils.h"
	"HelperFunctions.h"
	"OpenXRHelper.h"
	"OpenXRDebugUtils.cpp"
)

set(SOURCES
	"OpenXRDebugUtils.cpp"
)

add_library(openxr ${HEADERS} ${SOURCES} )

target_include_directories(openxr PUBLIC "${PROJECT_SOURCE_DIR}")


install(TARGETS openxr DESTINATION lib)
install(FILES ${HEADERS} ${SOURCES} DESTINATION include)