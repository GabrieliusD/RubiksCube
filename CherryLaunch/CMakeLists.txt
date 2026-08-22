cmake_minimum_required(VERSION 3.22.1)

project(CherryLaunch LANGUAGES CXX)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE CherryEngine)

if(CHERRY_MONOLITHIC)
	target_link_libraries(${PROJECT_NAME} PRIVATE ${CHERRY_ACTIVE_GAME_TARGET})
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=1)
else()
	target_compile_definitions(${PROJECT_NAME} PRIVATE CHERRY_MONOLITHIC=0 CHERRY_ACTIVE_GAME_MODULE="${CHERRY_ACTIVE_GAME_TARGET}.dll")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:CherryEngine> $<TARGET_FILE_DIR:${PROJECT_NAME}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:${CHERRY_ACTIVE_GAME_TARGET}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
	)
endif()

# Ensure runtime dependencies and assets are next to launcher
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/CherryEngine/thirdparty/dxc/bin/x64/dxcompiler.dll"
		$<TARGET_FILE_DIR:${PROJECT_NAME}>
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/CherryEngine/thirdparty/dxc/bin/x64/dxil.dll"
		$<TARGET_FILE_DIR:${PROJECT_NAME}>
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_SOURCE_DIR}/CherryEngine/Shaders"
		$<TARGET_FILE_DIR:${PROJECT_NAME}>/Shaders
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_SOURCE_DIR}/CherryEngine/Textures"
		$<TARGET_FILE_DIR:${PROJECT_NAME}>/Textures
)
