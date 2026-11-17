cmake_minimum_required(VERSION 3.22.1)

project(CherryLaunch LANGUAGES CXX)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE CherryEngine)

get_target_property(_cherry_game_source_dir ${CHERRY_ACTIVE_GAME_TARGET} SOURCE_DIR)

if(CHERRY_MONOLITHIC)
	# Engine and game are static libraries linked into this executable.
	target_link_libraries(${PROJECT_NAME} PRIVATE ${CHERRY_ACTIVE_GAME_TARGET})
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=1)
else()
	# Modular: the launcher loads the game DLL at runtime, so it only needs it built.
	# All DLLs land next to the launcher via CMAKE_RUNTIME_OUTPUT_DIRECTORY.
	add_dependencies(${PROJECT_NAME} ${CHERRY_ACTIVE_GAME_TARGET})
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=0 CHERRY_ACTIVE_GAME_MODULE="${CHERRY_ACTIVE_GAME_TARGET}.dll")
endif()

# Runtime dependencies (shaders, DXC) and the game's content next to the launcher.
cherry_add_runtime_assets(${PROJECT_NAME} "${_cherry_game_source_dir}")

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
