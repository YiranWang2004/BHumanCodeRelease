#include "Runtime.h"
#include "DancerKick.h"
#include "Representations/BehaviorControl/SkillRequest.h"
#include "Representations/Configuration/FieldDimensions.h"
#include "Framework/Blackboard.h"
#include "Framework/Configuration.h"
#include "Tools/Framework/BHExecutionUnit.h"
#include "Representations/Communication/BHumanMessageOutputGenerator.h"
#include "Representations/Communication/TeamData.h"
#include "Representations/Communication/GameControllerData.h"
#include "Representations/BehaviorControl/StrategyStatus.h"
#include "Representations/Infrastructure/FrameInfo.h"
#include "Representations/Infrastructure/SensorData/InertialSensorData.h"
#include "Representations/Sensing/InertialData.h"
#include "Math/Rotation.h"
#include "Platform/SystemCall.h"
#include "Platform/Time.h"
#include "Streaming/Global.h"
#include <chrono>
#include <iostream>

// Keep the original 2026 Color/Cognition/Motion module graphs and inter-thread
// representations. Only their frame source is replaced with server frames.
class HSLFrame : public BHExecutionUnit
{
  int stage = 0;
  unsigned sequence = 0;
  unsigned motionTick = 0;
  bool executed = false, activated = false;
  const LoggingController* initLogging(const Configuration& config, const size_t index) override
  {
    const auto& name = config()[index].name;
    stage = name == "Color" ? 1 : name == "Cognition" ? 2 : 3;
    return nullptr;
  }
  bool beforeFrame() override
  {
    auto& r = hsl::Runtime::get();
    std::unique_lock<std::mutex> lock(r.mutex);
    r.changed.wait(lock, [&] { return !r.running || (r.stage == stage && (r.sequence > sequence || (stage == 3 && motionTick < static_cast<unsigned>(r.options.motionSubframes)))); });
    if(r.sequence > sequence) motionTick = 0;
    hsl::sensorTimestamp = r.timestamp + (stage == 3 ? motionTick * (20u / r.options.motionSubframes) : 0u);
    executed = false;
    return r.running;
  }
  void beforeModules() override
  {
    if(stage == 3)
      Time::addSimulatedTime(static_cast<int>(hsl::sensorTimestamp - Time::getCurrentSystemTime()));
    BHExecutionUnit::beforeModules();
    SystemCall::mute(true);
    auto& r = hsl::Runtime::get();
    if(stage == 2 && !activated)
    {
      // Equivalent to the normal simulator's "GameStateProvider:active"
      // command. No chest button is present on a headless simulated agent.
      Global::getDebugRequestTable().addRequest(DebugRequest("module:GameStateProvider:active"));
      activated = true;
    }
    if(stage == 3 && Blackboard::getInstance().exists("MotionRequest"))
    {
      auto& request = static_cast<MotionRequest&>(Blackboard::getInstance()["MotionRequest"]);
      if(r.options.debugWalk)
      {
        request.motion = MotionRequest::walkAtAbsoluteSpeed;
        request.walkSpeed = r.options.debugVelocity;
      }
      else if(r.options.debugStand) request.motion = MotionRequest::stand;
      const bool otherKickoff = r.frame.mode.rfind("KickOff", 0) == 0 &&
        ((r.frame.mode.find("Left") != std::string::npos) != r.left);
      const bool nativeKickoffWait = Blackboard::getInstance().exists("GameState") &&
        static_cast<const GameState&>(Blackboard::getInstance()["GameState"]).state == GameState::opponentKickOff;
      if(otherKickoff || nativeKickoffWait || r.frame.penalized) request.motion = MotionRequest::stand;
      if(r.options.kickPolicy == "zjudancer")
      {
        if(motionTick == 0) r.dancerMotionRequest = request;
        if(r.dancerHolding) request.motion = MotionRequest::stand;
        else if(request.motion == MotionRequest::walkToBallAndKick || request.motion == MotionRequest::dribble ||
                (r.ownSetPlayKicker && r.kickAllowed && !r.frame.stopped && !r.frame.penalized))
        {
          // Suppress ALL native shots/passes, including during cooldown. Keep
          // native path planning for approach when the visual gate is closed.
          request.motion = MotionRequest::walkToPose;
          const auto& ball = r.ownSetPlayKicker ? r.ball.estimate.position : request.ballEstimate.position;
          request.walkTarget = Pose2f(ball.angle(), ball);
          if(r.ownSetPlayKicker)
          {
            request.walkSpeed = Pose2f(1.f, 1.f, 1.f);
            // The old global path led to the native pass waiting pose. Keep
            // local avoidance, but don't follow that obsolete path.
            request.obstacleAvoidance.path.clear();
          }
        }
      }
    }
  }
  void afterModules() override
  {
    auto& r = hsl::Runtime::get();
    auto& blackboard = Blackboard::getInstance();
    const auto copy = [&](const char* name, auto& destination)
    {
      using T = std::remove_reference_t<decltype(destination)>;
      if(blackboard.exists(name)) destination = static_cast<const T&>(blackboard[name]);
    };
    if(stage == 2)
    {
      if(blackboard.exists("OdometryData")) r.cognitionOdometry = static_cast<const OdometryData&>(blackboard["OdometryData"]);
      copy("RobotPose", r.estimate);
      copy("BallModel", r.ball);
      if(blackboard.exists("FieldDimensions"))
      {
        const auto& field = static_cast<const FieldDimensions&>(blackboard["FieldDimensions"]);
        r.goalX = field.xPosOpponentGoalLine * .001f;
        r.fieldHalfWidth = field.yPosLeftTouchline * .001f;
        r.goalieAreaX = field.xPosOwnPenaltyArea * .001f + 1.f;
        r.goalieAreaY = field.yPosLeftPenaltyArea * .001f + .25f;
      }
      if(blackboard.exists("StrategyStatus"))
      {
        const auto& status = static_cast<const StrategyStatus&>(blackboard["StrategyStatus"]);
        r.tactic = TypeRegistry::getEnumName(status.acceptedTactic);
        r.position = TypeRegistry::getEnumName(status.position);
        r.behaviorRole = TypeRegistry::getEnumName(status.role);
        r.ballOwner = r.behaviorRole == "playBall" || r.behaviorRole == "kickOffForward";
      }
      if(blackboard.exists("GameControllerData"))
      {
        const auto& data = static_cast<const GameControllerData&>(blackboard["GameControllerData"]);
        r.gcState = data.state; r.gcTime = data.timeLastPacketReceived;
      }
      if(blackboard.exists("FrameInfo")) r.cognitionTime = static_cast<const FrameInfo&>(blackboard["FrameInfo"]).time;
      r.ownSetPlayKicker = false;
      if(blackboard.exists("GameState"))
      {
        const auto& state = static_cast<const GameState&>(blackboard["GameState"]);
        r.gameState = state.state; r.playerState = state.playerState;
        if(state.isPlaying() && state.isForOwnTeam() && blackboard.exists("SkillRequest"))
        {
          // All own restarts: kick-in, goal/corner/free kicks, kick-off and
          // penalties. Strategy has chosen the taker, before PassToTeammate's
          // walk-to-waiting-pose / wait / adjust states produce a kick motion.
          const auto skill = static_cast<const SkillRequest&>(blackboard["SkillRequest"]).skill;
          r.ownSetPlayKicker = skill == SkillRequest::pass || skill == SkillRequest::shoot ||
            skill == SkillRequest::waitShoot || skill == SkillRequest::clear || skill == SkillRequest::dribble;
        }
        r.kickAllowed = state.isPlaying() && !state.isPenalized() &&
          state.state != GameState::opponentKickOff &&
          (state.state == GameState::playing || state.isForOwnTeam());
      }
      if(blackboard.exists("TeamData"))
      {
        const auto& data = static_cast<const TeamData&>(blackboard["TeamData"]);
        r.teammates = static_cast<int>(data.teammates.size());
        r.teamReceived = data.receivedMessages;
        r.teamUnsynchronized = data.receivedUnsynchronizedMessages;
      }
      if(blackboard.exists("BHumanMessageOutputGenerator"))
      {
        const auto& message = static_cast<const BHumanMessageOutputGenerator&>(blackboard["BHumanMessageOutputGenerator"]);
        if(message.send && message.sendThisFrame && message.sendThisFrame()) message.send();
        r.teamSent = message.sentMessages;
      }
    }
    if(stage == 3)
    {
      ++motionTick;
      ++r.motionTicks;
      copy("JointRequest", r.request);
      copy("JointCalibration", r.calibration);
      copy("JointAngles", r.measurements);
      copy("MotionRequest", r.motion);
      copy("MotionInfo", r.motionInfo);
      if(blackboard.exists("InertialSensorData")) r.imuYaw = static_cast<const InertialSensorData&>(blackboard["InertialSensorData"]).angle.z();
      if(blackboard.exists("InertialData")) r.nativeYaw = Rotation::Euler::getZAngle(static_cast<const InertialData&>(blackboard["InertialData"]).orientation3D);
      if(blackboard.exists("OdometryData")) r.odometry = static_cast<const OdometryData&>(blackboard["OdometryData"]);
      // Run shared body policies after the native planner has produced this
      // frame's velocity. Native fall/get-up/prepare phases keep motor control.
      if(r.options.kickPolicy == "zjudancer" && motionTick == static_cast<unsigned>(r.options.motionSubframes))
      {
        try { hsl::stepDancerKick(r.dancerMotionRequest); }
        catch(const std::exception& error)
        {
          r.dancerFault = error.what();
          r.dancerAction.clear();
          r.stop();
        }
      }
    }
    sequence = r.sequence;
    executed = true;
  }
  bool afterFrame() override
  {
    auto& r = hsl::Runtime::get();
    if(executed)
    {
      std::lock_guard<std::mutex> lock(r.mutex);
      if(stage == 3)
      {
        if(motionTick == static_cast<unsigned>(r.options.motionSubframes)) { r.completed = sequence; r.stage = 0; }
      }
      else r.stage = stage + 1;
      r.changed.notify_all();
    }
    return false;
  }
};
REGISTER_EXECUTION_UNIT(HSLFrame)

// HSL matches can share a host without sharing native team multicast channels.
int bhumanHSLTeamPort()
{
  const auto& options = hsl::Runtime::get().options;
  return options.teamPort ? options.teamPort : Settings::getPortForTeam(options.teamId);
}
