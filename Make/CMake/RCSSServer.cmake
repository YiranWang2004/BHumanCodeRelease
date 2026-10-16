# Observational callbacks only; the native 2026 policy and switching logic are unchanged.
target_compile_definitions(B-Human PRIVATE BHUMAN_RCSS_SERVER)
set(RCSS_SERVER_ROOT "${BHUMAN_PREFIX}/Src/Apps/RCSSServer")
file(GLOB RCSS_SERVER_SOURCES CONFIGURE_DEPENDS "${RCSS_SERVER_ROOT}/*.cpp")
add_executable(run_player ${RCSS_SERVER_SOURCES})
target_link_libraries(run_player PRIVATE B-Human)
target_include_directories(run_player PRIVATE "${RCSS_SERVER_ROOT}")
target_compile_definitions(run_player PRIVATE RCSS_ASSETS_DIR="${BHUMAN_PREFIX}")
set_target_properties(run_player PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

enable_testing()
add_executable(rcss_protocol_test "${RCSS_SERVER_ROOT}/Protocol.cpp"
    "${RCSS_SERVER_ROOT}/tests/ProtocolTest.cpp")
target_include_directories(rcss_protocol_test PRIVATE "${RCSS_SERVER_ROOT}")
add_test(NAME rcss_protocol COMMAND rcss_protocol_test)

add_executable(rcss_imu_test "${RCSS_SERVER_ROOT}/tests/IMUOrientationTest.cpp")
target_include_directories(rcss_imu_test PRIVATE "${RCSS_SERVER_ROOT}")
target_link_libraries(rcss_imu_test PRIVATE Math Flags::Default)
add_test(NAME rcss_imu COMMAND rcss_imu_test)
