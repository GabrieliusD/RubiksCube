set(HEADERS
	imconfig.h
	imgui.h
	imgui_impl_dx12.h
	imgui_impl_win32.h
	imgui_internal.h
	imstb_rectpack.h
	imstb_textedit.h
	imstb_truetype.h
)

set(SOURCES
	imgui.cpp
	imgui_demo.cpp
	imgui_draw.cpp
	imgui_impl_dx12.cpp
	imgui_impl_win32.cpp
	imgui_tables.cpp
	imgui_widgets.cpp
)

add_library(imgui ${HEADERS} ${SOURCES})

install(TARGETS imgui DESTINATION lib)
install(FILES ${HEADERS} ${SOURCES} DESTINATION include)