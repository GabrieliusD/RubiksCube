cmake_minimum_required(VERSION 3.22.1)

project(CherryLaunch LANGUAGES C CXX)

if(ANDROID)
	# The app's native library (lib<OUTPUT_NAME>.so, named in AndroidManifest.xml): NativeActivity
	# glue, the engine and the active game, all static.
	if(NOT CHERRY_MONOLITHIC)
		message(FATAL_ERROR "Android builds are monolithic: configure with -DCHERRY_MONOLITHIC=ON")
	endif()
	set(_glue "${ANDROID_NDK}/sources/android/native_app_glue")
	add_library(${PROJECT_NAME} SHARED AndroidMain.cpp "${_glue}/android_native_app_glue.c")
	target_include_directories(${PROJECT_NAME} PRIVATE "${_glue}")
	set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "game")
	# NativeActivity looks up ANativeActivity_onCreate, which the glue defines.
	target_link_options(${PROJECT_NAME} PRIVATE "-u" "ANativeActivity_onCreate")
	target_link_libraries(${PROJECT_NAME} PRIVATE CherryEngine
		"$<LINK_LIBRARY:WHOLE_ARCHIVE,${CHERRY_ACTIVE_GAME_TARGET}>" android log)
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=1)
	return()
endif()

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE CherryEngine)

if(CHERRY_MONOLITHIC)
	# Engine and game are static libraries linked into this executable.
	target_link_libraries(${PROJECT_NAME} PRIVATE ${CHERRY_ACTIVE_GAME_TARGET})
	# Scripts register themselves from static initialisers that nothing references, so the
	# linker would drop them from a static library: keep the whole game library.
	if(MSVC)
		target_link_options(${PROJECT_NAME} PRIVATE "/WHOLEARCHIVE:${CHERRY_ACTIVE_GAME_TARGET}")
	endif()
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=1)
else()
	# Modular: the engine loads the open project's game DLL at runtime, so every game
	# module is built and lands next to the launcher via CMAKE_RUNTIME_OUTPUT_DIRECTORY.
	get_property(_cherry_games GLOBAL PROPERTY CHERRY_GAME_MODULES)
	add_dependencies(${PROJECT_NAME} ${_cherry_games})
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=0)
endif()

# Runtime dependencies (shaders, DXC; the game's content in monolithic builds) next to the launcher.
cherry_add_runtime_assets(${PROJECT_NAME})

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
