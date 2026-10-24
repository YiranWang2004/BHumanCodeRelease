#include "DancerPolicy.h"
#include "DancerKickGate.h"
#include "DancerGeometry.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace
{
struct Controller
{
  rcss::DancerPolicy walk, kick;
  rcss::DancerKickGate gate;
  unsigned lastSequence = 0;
  unsigned lastTime = 0;
  bool wasKicking = false;
  bool hadFrame = false, detected = false, haveBall = false;
  unsigned lastSeen = 0, lastVision = 0;
  std::array<double, 3> visualBall{};
  double worldKickDirection = 0.;
  std::array<double, 3> lastPosition{};
  rcss::DancerPolicy::Command command;
  explicit Controller(const std::string& directory) :
    walk(directory, rcss::DancerPolicy::Kind::walk), kick(directory, rcss::DancerPolicy::Kind::kick) {}
};
Controller* activeController = nullptr;
Controller& controller()
{
  static Controller instance(rcss::Runtime::get().options.assets + "/Config/NeuralNets/Dancer");
  return instance;
}
}

float bhumanRCSSDancerForwardSpeed()
{
  const auto& options = rcss::Runtime::get().options;
  // The faster deployment was tested on standard K1. Preserve the planner's
  // existing parameters for native motion and for the other server models.
  if(options.motionPolicy != "dancer" || options.robot != "K1" || options.serverModel != "K1") return 0.f;
  return static_cast<float>(controller().walk.velocityLimits()[0] * 1000.);
}

void bhumanRCSSDancerMotionInterrupted()
{
  auto& r = rcss::Runtime::get();
  if(r.options.motionPolicy != "dancer" || !activeController || !activeController->gate.session()) return;
  auto& c = *activeController;
  c.gate.interrupt(r.timestamp, "motion_interrupted");
  c.wasKicking = false;
  r.dancerGate = c.gate.reason; r.dancerExitReason = c.gate.lastExitReason;
}

bool bhumanRCSSDancerActive()
{
  const auto& r = rcss::Runtime::get();
  return r.options.motionPolicy == "dancer" && r.dancerSequence == r.sequence && r.dancerCommand;
}

bool bhumanRCSSDancerStep(bool entryRequested, bool standing, const Pose2f& speed,
                         float direction, const Vector2f& ball, unsigned nativeFlags, JointAngles& target, bool& kicking)
{
  auto& r = rcss::Runtime::get();
  if(r.options.motionPolicy != "dancer" || r.frame.time < 0.) return false;
  auto& c = controller();
  activeController = &c;
  if(c.lastSequence != r.sequence || !c.hadFrame)
  {
    rcss::DancerPolicy::State state;
    const auto& names = rcss::sensorNames();
    for(size_t i = 0; i < names.size(); ++i)
    {
      const auto joint = std::find_if(r.frame.joints.begin(), r.frame.joints.end(),
                                    [&](const rcss::Joint& j) { return j.name == names[i]; });
      if(joint == r.frame.joints.end()) throw std::runtime_error("Missing dancer joint measurement");
      // K1BHuman ref angles cancel its XML body's rest rotation. The actual
      // sensor/command coordinates equal Booster's serial coordinates; adding
      // another arm-zero offset here would rotate the arms twice.
      state.q[i] = joint->q; state.dq[i] = joint->dq;
    }
    Quaternionf orientation(static_cast<float>(r.frame.quaternion[0]), static_cast<float>(r.frame.quaternion[1]),
                            static_cast<float>(r.frame.quaternion[2]), static_cast<float>(r.frame.quaternion[3]));
    orientation.normalize();
    const Vector3f gravity = orientation.conjugate() * Vector3f(0.f, 0.f, -1.f);
    for(size_t i = 0; i < 3; ++i) { state.gravity[i] = gravity[static_cast<int>(i)]; state.gyro[i] = r.frame.gyro[i]; }
    if(!standing) state.velocity = {speed.translation.x() * .001, speed.translation.y() * .001, static_cast<float>(speed.rotation)};

    if(r.frame.hasVision)
    {
      const rcss::Object* ball = nullptr;
      for(const auto& object : r.frame.objects)
        if(object.type == "B" && object.distance > 0. && (!ball || object.distance < ball->distance)) ball = &object;
      if(ball)
      {
        const double az = ball->azimuth * Constants::pi / 180., el = ball->elevation * Constants::pi / 180.;
        // Camera site for the selected server model. Retain the full tilted
        // torso frame, rather than BallModel's horizontal ground projection.
        state.ball = rcss::dancerCameraBall(state.q[0], state.q[1], ball->distance, az, el, r.options.serverModel == "K1");
        state.seesBall = true;
      }
    }
    const double displacement = std::hypot(r.frame.position[0] - c.lastPosition[0], r.frame.position[1] - c.lastPosition[1]);
    const bool reset = !c.hadFrame || r.sequence != c.lastSequence + 1 || displacement > 2.;
    if(reset)
    {
      c.walk.reset(); c.kick.reset();
      if(!c.hadFrame || displacement > 2.) c.gate.reset();
      else c.gate.interrupt(c.lastTime + 20, "motion_interrupted"); // fall/get-up/playDead retain priority
      c.wasKicking = false; c.haveBall = c.detected = false;
    }
    // See arrives every 40 ms, policies every 20 ms. Keep detection until the
    // next See message, exactly as dancer's vision callback/brain tick does.
    if(r.frame.hasVision) { c.detected = state.seesBall; c.lastVision = r.timestamp; }
    if(state.seesBall)
    {
      c.visualBall = state.ball; c.lastSeen = r.timestamp; c.haveBall = true;
    }
    if(entryRequested || !c.gate.session()) c.worldKickDirection = direction + r.estimate.rotation;
    const double localDirection = c.worldKickDirection - r.estimate.rotation;
    // Keep the tactical direction through native interception/walk requests.
    // Rotate horizontal heading back into the tilted torso, as for ball input.
    const auto tilt = r.tilt();
    const Matrix3f roll = (AngleAxisf(tilt.x(), Vector3f::UnitX())).toRotationMatrix();
    const Matrix3f pitch = (AngleAxisf(tilt.y(), Vector3f::UnitY())).toRotationMatrix();
    const Vector3f bodyDirection = roll.transpose() * pitch.transpose() * Vector3f(static_cast<float>(std::cos(localDirection)), static_cast<float>(std::sin(localDirection)), 0.f);
    state.direction = {bodyDirection.x(), bodyDirection.y()};

    rcss::DancerKickGate::Input input;
    input.now = r.timestamp;
    input.entryRequested = entryRequested;
    input.teamAllowed = r.dancerBallOwner;
    input.hardStop = !r.dancerGameAllowed || r.dancerForcedStop;
    const unsigned age = r.timestamp - c.lastSeen;
    input.detected = c.haveBall && c.detected && age <= 650 && r.timestamp - c.lastVision <= 1000;
    input.visualRange = std::hypot(c.visualBall[0], c.visualBall[1]);
    input.bearing = std::atan2(c.visualBall[1], c.visualBall[0]);
    input.ballX = ball.x() * .001; input.ballY = ball.y() * .001;
    const double range = std::hypot(input.ballX, input.ballY);
    // Match dancer's fresh observation, 1 s memory, near unseen-ball memory
    // rejection and then 1 s unusable confirmation. No global ball truth.
    input.ballUsable = (input.detected && input.visualRange > .05) ||
      (c.haveBall && age <= 1000 && std::isfinite(range) &&
       (input.detected || (input.visualRange > .45 && range > .45)));
    const Vector2f fieldBall = r.estimate * ball;
    input.fieldBallX = fieldBall.x() * .001; input.fieldBallY = fieldBall.y() * .001;
    input.robotX = r.estimate.translation.x() * .001; input.robotY = r.estimate.translation.y() * .001;
    input.robotYaw = r.estimate.rotation; input.kickDirection = c.worldKickDirection;
    const bool useKick = c.gate.update(input);
    if(c.gate.zeroVelocity) state.velocity = {0., 0., 0.};
    if(useKick != c.wasKicking) { c.walk.reset(); c.kick.reset(); }
    c.command = (useKick ? c.kick : c.walk).step(state);
    r.dancerVelocity = state.velocity;
    r.dancerKickDirectionWorld = c.worldKickDirection;
    c.wasKicking = useKick; c.hadFrame = true; c.lastSequence = r.sequence; c.lastTime = r.timestamp; c.lastPosition = r.frame.position;
    r.dancerKp = c.command.kp; r.dancerKd = c.command.kd;
    r.dancerCommand = true; r.dancerSequence = r.sequence;
    r.policy = useKick ? "5070Ti_equivariant_student.onnx" : "2026-10-04_10-24-37.onnx";
    r.policyKind = useKick ? "kick" : "walk";
    r.policyTime = rcss::sensorTimestamp;
    if(useKick) ++r.kickInferences; else ++r.walkInferences;
    r.dancerGate = c.gate.reason;
    r.dancerExitReason = c.gate.lastExitReason;
    r.dancerNativeFlags = nativeFlags;
    // Old kick flags have no meaning for this actor. Explicit availability
    // distinguishes the fixed velocity command from a requested ball speed.
    r.kickStrong = r.kickInaccurate = r.kickAllowDeviation = r.kickTargetSpeed = 0.f;
  }
  for(size_t i = 2; i < 22; ++i) target.angles[r.motors.jointMapping[i]] = static_cast<float>(c.command.q[i]);
  kicking = c.wasKicking;
  return true;
}
