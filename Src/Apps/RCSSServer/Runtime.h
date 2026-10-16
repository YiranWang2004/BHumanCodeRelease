#pragma once
#include "Protocol.h"
#include "Math/Pose2f.h"
#include "Representations/Infrastructure/JointRequest.h"
#include "Representations/Infrastructure/JointAngles.h"
#include "Representations/Configuration/JointCalibration.h"
#include "Representations/Configuration/MotorParameters.h"
#include "Representations/MotionControl/MotionRequest.h"
#include "Representations/MotionControl/MotionInfo.h"
#include "Representations/Modeling/BallModel.h"
#include "Representations/Modeling/RobotPose.h"
#include "Representations/Configuration/FieldDimensions.h"
#include <condition_variable>
#include <mutex>

namespace rcss
{
struct Options
{
  std::string host = "127.0.0.1", team = "BHuman", field = "fifa7vs7", localization = "truth", role;
  std::string assets = RCSS_ASSETS_DIR, trace, robot = "K1", kickMode = "fast-max";
  int port = 60000, teamId = 2, number = 1, players = 7, maxFrames = 0, motionSubframes = 10;
  std::string monitorHost = "127.0.0.1";
  int monitorPort = 10190;
  double speed = 1., beamX = 0., beamY = 0., beamYaw = 0.;
  bool beamExplicit = false, debugWalk = false, debugStand = false, log = false, monitor = false;
  Pose2f debugVelocity;
};
Options arguments(int argc, char** argv);

struct BehaviorNode
{
  std::string option, state;
  int depth = 0, optionTime = 0, stateTime = 0;
  std::vector<std::string> arguments;
};

STREAMABLE(JointMapping,
{,
  (std::vector<Joints::Joint>) jointMapping,
});

struct Runtime
{
  static Runtime& get();
  Options options;
  Frame frame;
  JointMapping motors;
  MotorParameters motorParameters;
  std::mutex mutex;
  std::condition_variable changed;
  bool running = true, left = true, sideKnown = false;
  int stage = 0;
  unsigned sequence = 0, completed = 0, timestamp = 100000, motionTicks = 0;
  JointRequest request;
  JointCalibration calibration;
  JointAngles measurements;
  RobotPose estimate;
  Pose2f odometry;
  MotionRequest motion;
  MotionInfo motionInfo;
  BallModel ball;
  int gameState = 0, playerState = 0, teammates = 0, gcState = 0;
  unsigned gcTime = 0, cognitionTime = 0;
  float imuYaw = 0.f, nativeYaw = 0.f;
  Pose2f cognitionOdometry;
  std::string policy = "none", tactic = "none", setPlay = "none";
  std::string behavior;
  std::vector<BehaviorNode> behaviorGraph;
  std::string role = "none", position = "none";
  std::vector<Vector2f> opponents;
  FieldDimensions fieldDimensions;
  std::string skill = "none", policyKind = "none";
  unsigned walkInferences = 0, kickInferences = 0, policyTime = 0;
  unsigned getUpInferences = 0;
  unsigned teamSent = 0, teamReceived = 0, teamUnsynchronized = 0;
  void stop();
  std::string action() const;
  Pose2f truth() const;
  Vector3a tilt() const;
};
// Server positions are in metres; B-Human representations are in millimetres.
// Measured vision uses only joint angles and roll/pitch (never torso x/y/yaw).
const std::vector<std::string>& sensorNames();
// Timestamp of the native thread's current frame. Motion subframes advance
// this clock while Color/Cognition keep the outer server frame timestamp.
extern thread_local unsigned sensorTimestamp;
}
