cmake_minimum_required(VERSION 3.16)
project(ecp_test_engine C)

add_subdirectory(../../sdk/c ecp)

add_executable(test-engine main.c)
target_link_libraries(test-engine PRIVATE ecp)
set_target_properties(test-engine PROPERTIES C_STANDARD 11)
if(WIN32)
  target_link_libraries(test-engine PRIVATE winmm)
endif()
if(MSVC)
  target_compile_definitions(test-engine PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(test-engine PRIVATE -Wall -Wextra)
endif()

# ecp.json beside the executable, for frontends that discover engines by file (SPEC section 2).
add_custom_command(TARGET test-engine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:test-engine> -DOUT=$<TARGET_FILE_DIR:test-engine>/ecp.json
          -P ${CMAKE_CURRENT_SOURCE_DIR}/describe.cmake)
