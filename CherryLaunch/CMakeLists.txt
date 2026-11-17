cmake_minimum_required(VERSION 3.22.1)

project(CherryLaunch LANGUAGES CXX)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE CherryEngine)

if(CHERRY_MONOLITHIC)
	# Engine and game are static libraries linked into this executable.
	target_link_libraries(${PROJECT_NAME} PRIVATE ${CHERRY_ACTIVE_GAME_TARGET})
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=1)
else()
	# Modular: the launcher loads a game DLL at runtime (CHERRY_ACTIVE_GAME_MODULE by
	# default, or the DLL named on the command line), so every game module is built
	# and lands next to it via CMAKE_RUNTIME_OUTPUT_DIRECTORY.
	get_property(_cherry_games GLOBAL PROPERTY CHERRY_GAME_MODULES)
	add_dependencies(${PROJECT_NAME} ${_cherry_games})
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=0 CHERRY_ACTIVE_GAME_MODULE="${CHERRY_ACTIVE_GAME_TARGET}.dll")
endif()

# Runtime dependencies (shaders, DXC) and the game's content next to the launcher.
cherry_add_runtime_assets(${PROJECT_NAME})

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
