# RCSS diagnostics and simulation-only kick command override.
target_compile_definitions(B-Human PRIVATE BHUMAN_RCSS_SERVER)
target_compile_definitions(OptionsAndSkills PRIVATE BHUMAN_RCSS_SERVER)
set(RCSS_SERVER_ROOT "${BHUMAN_PREFIX}/Src/Apps/RCSSServer")
file(GLOB RCSS_SERVER_SOURCES CONFIGURE_DEPENDS "${RCSS_SERVER_ROOT}/*.cpp")
add_executable(run_player ${RCSS_SERVER_SOURCES})
target_link_libraries(run_player PRIVATE B-Human ONNXRuntime::ONNXRuntime)
target_include_directories(run_player SYSTEM PRIVATE "${BHUMAN_PREFIX}/Util/BoosterRoboticsSDK/include")
target_include_directories(run_player PRIVATE "${RCSS_SERVER_ROOT}")
target_compile_definitions(run_player PRIVATE BHUMAN_RCSS_SERVER RCSS_ASSETS_DIR="${BHUMAN_PREFIX}")
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

# Exercise the actual team-message codec and location-specific schema, including
# the original default schema as a negative control for the field-edge bug.
add_executable(rcss_team_message_test "${RCSS_SERVER_ROOT}/tests/TeamMessageTest.cpp"
    "${RCSS_SERVER_ROOT}/Runtime.cpp" "${RCSS_SERVER_ROOT}/Protocol.cpp")
target_link_libraries(rcss_team_message_test PRIVATE B-Human)
target_include_directories(rcss_team_message_test PRIVATE "${RCSS_SERVER_ROOT}")
target_compile_definitions(rcss_team_message_test PRIVATE BHUMAN_RCSS_SERVER RCSS_ASSETS_DIR="${BHUMAN_PREFIX}")
add_test(NAME rcss_team_message COMMAND rcss_team_message_test)

add_executable(rcss_formation_test "${RCSS_SERVER_ROOT}/tests/FormationTest.cpp"
    "${RCSS_SERVER_ROOT}/Runtime.cpp" "${RCSS_SERVER_ROOT}/Protocol.cpp")
target_link_libraries(rcss_formation_test PRIVATE B-Human)
target_include_directories(rcss_formation_test PRIVATE "${RCSS_SERVER_ROOT}")
target_compile_definitions(rcss_formation_test PRIVATE BHUMAN_RCSS_SERVER RCSS_ASSETS_DIR="${BHUMAN_PREFIX}")
add_test(NAME rcss_formation COMMAND rcss_formation_test)

add_executable(rcss_dancer_adapter_test "${RCSS_SERVER_ROOT}/tests/DancerAdapterTest.cpp"
    "${RCSS_SERVER_ROOT}/Runtime.cpp" "${RCSS_SERVER_ROOT}/Protocol.cpp")
target_link_libraries(rcss_dancer_adapter_test PRIVATE B-Human)
target_include_directories(rcss_dancer_adapter_test PRIVATE "${RCSS_SERVER_ROOT}")
target_compile_definitions(rcss_dancer_adapter_test PRIVATE BHUMAN_RCSS_SERVER RCSS_ASSETS_DIR="${BHUMAN_PREFIX}")
add_test(NAME rcss_dancer_adapter COMMAND rcss_dancer_adapter_test)

foreach(RCSS_TEST rcss_team_message_test rcss_formation_test rcss_dancer_adapter_test)
  target_sources(${RCSS_TEST} PRIVATE "${RCSS_SERVER_ROOT}/DancerAdapter.cpp" "${RCSS_SERVER_ROOT}/DancerPolicy.cpp"
      "${RCSS_SERVER_ROOT}/DancerGetUp.cpp" "${RCSS_SERVER_ROOT}/DancerRecovery.cpp")
  target_include_directories(${RCSS_TEST} SYSTEM PRIVATE "${BHUMAN_PREFIX}/Util/BoosterRoboticsSDK/include")
  target_link_libraries(${RCSS_TEST} PRIVATE ONNXRuntime::ONNXRuntime)
endforeach()

# Standalone deployment probe: also used for numerical parity against the
# original Python AMP implementation and exported VisualKick model.
add_executable(rcss_dancer_policy_probe "${RCSS_SERVER_ROOT}/DancerPolicy.cpp" "${RCSS_SERVER_ROOT}/DancerRecovery.cpp"
    "${RCSS_SERVER_ROOT}/tests/DancerPolicyProbe.cpp")
target_include_directories(rcss_dancer_policy_probe PRIVATE "${RCSS_SERVER_ROOT}")
target_include_directories(rcss_dancer_policy_probe SYSTEM PRIVATE "${BHUMAN_PREFIX}/Util/BoosterRoboticsSDK/include")
target_link_libraries(rcss_dancer_policy_probe PRIVATE ONNXRuntime::ONNXRuntime)

add_executable(rcss_dancer_gate_test "${RCSS_SERVER_ROOT}/tests/DancerKickGateTest.cpp")
target_include_directories(rcss_dancer_gate_test PRIVATE "${RCSS_SERVER_ROOT}")
add_test(NAME rcss_dancer_gate COMMAND rcss_dancer_gate_test)

# Offline range experiment; consumes the same See messages as run_player.
add_executable(rcss_decode_players "${RCSS_SERVER_ROOT}/Protocol.cpp"
    "${RCSS_SERVER_ROOT}/experiments/DecodePlayers.cpp")
target_include_directories(rcss_decode_players PRIVATE "${RCSS_SERVER_ROOT}")
