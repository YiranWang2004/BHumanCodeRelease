# Native inference callbacks are observational. The optional ZJUDancer override
# lives exclusively in the HSL adapter; normal B-Human builds are unchanged.
target_compile_definitions(B-Human PRIVATE BHUMAN_HSL_SERVER)
set(HSL_SERVER_ROOT "${BHUMAN_PREFIX}/Src/Apps/HSLServer")
file(GLOB HSL_SERVER_SOURCES CONFIGURE_DEPENDS "${HSL_SERVER_ROOT}/*.cpp")
add_executable(run_player ${HSL_SERVER_SOURCES})
target_link_libraries(run_player PRIVATE B-Human)
target_include_directories(run_player PRIVATE "${HSL_SERVER_ROOT}")
target_compile_definitions(run_player PRIVATE HSL_ASSETS_DIR="${BHUMAN_PREFIX}")
set_target_properties(run_player PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

enable_testing()
add_executable(hsl_protocol_test "${HSL_SERVER_ROOT}/Protocol.cpp"
    "${HSL_SERVER_ROOT}/tests/ProtocolTest.cpp")
target_include_directories(hsl_protocol_test PRIVATE "${HSL_SERVER_ROOT}")
add_test(NAME hsl_protocol COMMAND hsl_protocol_test)

add_executable(hsl_imu_test "${HSL_SERVER_ROOT}/tests/IMUOrientationTest.cpp")
target_include_directories(hsl_imu_test PRIVATE "${HSL_SERVER_ROOT}")
target_link_libraries(hsl_imu_test PRIVATE Math Flags::Default)
add_test(NAME hsl_imu COMMAND hsl_imu_test)
