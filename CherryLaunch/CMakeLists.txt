cmake_minimum_required(VERSION 3.22.1)

project(CherryLaunch LANGUAGES CXX)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE CherryEngine)

if(CHERRY_MONOLITHIC)
	target_link_libraries(${PROJECT_NAME} PRIVATE ${CHERRY_ACTIVE_GAME_TARGET})
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=1)
else()
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=0 CHERRY_ACTIVE_GAME_MODULE="${CHERRY_ACTIVE_GAME_TARGET}.dll")
	cherry_copy_target_runtime(${PROJECT_NAME} CherryEngine)
	cherry_copy_target_runtime(${PROJECT_NAME} ${CHERRY_ACTIVE_GAME_TARGET})
endif()

# Ensure runtime dependencies and assets are next to launcher
cherry_copy_runtime_asset_bundle(${PROJECT_NAME})
