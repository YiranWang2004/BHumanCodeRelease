#include "DancerRecovery.h"
#include "Runtime.h"
#include "Representations/MotionControl/WalkGenerator.h"
#include "Tools/Motion/MotionPhase.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
struct DancerGetUpPhase : MotionPhase
{
  rcss::DancerRecovery policy;
  const WalkGenerator& walkGenerator;
  unsigned sequence = 0, finishedSequence = 0, attempts = 1;
  bool inferred = false, finished = false, retry = false;

  explicit DancerGetUpPhase(const WalkGenerator& generator) : MotionPhase(MotionPhase::getUp),
    policy(rcss::Runtime::get().options.assets + "/Config/NeuralNets/Dancer"), walkGenerator(generator) {}

  void update() override
  {
    auto& r = rcss::Runtime::get();
    if((inferred && sequence == r.sequence) || finished) return;
    if(retry) { policy.reset(); retry = false; }
    rcss::DancerPolicy::State state;
    const auto& names = rcss::sensorNames();
    for(size_t i = 0; i < 22; ++i)
    {
      const auto j = std::find_if(r.frame.joints.begin(), r.frame.joints.end(), [&](const rcss::Joint& j) { return j.name == names[i]; });
      if(j == r.frame.joints.end()) throw std::runtime_error("Missing getup joint sensor");
      state.q[i] = j->q; state.dq[i] = j->dq;
    }
    auto q = r.frame.quaternion;
    double norm = 0.; for(double v : q) norm += v*v;
    if(!std::isfinite(norm) || norm < 1.e-16) throw std::runtime_error("Invalid getup orientation");
    for(double& v : q) v /= std::sqrt(norm);
    const auto [w,x,y,z] = q;
    state.gravity = {2*(w*y-x*z), -2*(w*x+y*z), 2*(x*x+y*y)-1};
    state.gyro = r.frame.gyro;
    const auto& output = policy.step(state, r.frame.position[2]);
    r.dancerRecoveryTau = output.tau; r.dancerRecoveryKd = output.kd;
    r.dancerRecoverySequence = r.sequence; r.dancerRecoveryCommand = true;
    r.dancerRecoveryRow = output.row; r.dancerRecoveryAttempts = attempts;
    r.dancerRecoveryState = output.status == rcss::DancerRecovery::Status::settling ? "settling" : "executing";
    if(output.status == rcss::DancerRecovery::Status::failed)
    {
      // Source SimController releases all torques for one control frame,
      // resets policy state, and starts another six-second attempt.
      r.dancerRecoveryTau.fill(0.f); r.dancerRecoveryKd.fill(0.f);
      r.dancerRecoveryState = "retrying"; retry = true; ++attempts;
    }
    else if(state.gravity[2] < -.85 && r.frame.position[2] >= .30)
    {
      finished = true; finishedSequence = r.sequence; r.dancerRecoveryState = "handoff";
    }
    r.policy = "model_4999.onnx"; r.policyKind = "getUp"; r.policyTime = rcss::sensorTimestamp;
    ++r.getUpInferences; sequence = r.sequence; inferred = true;
  }

  bool isDone(const MotionRequest& request) const override
  {
    return request.motion == MotionRequest::playDead || request.motion == MotionRequest::prepare ||
      (finished && rcss::Runtime::get().sequence > finishedSequence);
  }

  void calcJoints(const MotionRequest&, JointRequest& joints, Pose2f& odometry, MotionInfo& info) override
  {
    update();
    joints.angles.fill(0.f); joints.stiffnessData.stiffnesses.fill(100);
    // The RCSS serializer supplies zero Kp, live Kd and held feedforward tau.
    // Never reinterpret torque as a position target in B-Human's joint stream.
    joints.isTorqueControl = false;
    info.isMotionStable = false; info.isWalkPhaseInWalkKick = false;
    info.speed = Pose2f(); info.getUpTryCounter = attempts;
    odometry = Pose2f();
  }

  std::unique_ptr<MotionPhase> createNextPhase(const MotionPhase& next) const override
  {
    if(next.type == MotionPhase::prepare || next.type == MotionPhase::playDead) return nullptr;
    return walkGenerator.createPhase(Pose2f(0.f, .01f, 0.f), *this, 0.f);
  }
};
}

std::unique_ptr<MotionPhase> bhumanRCSSDancerGetUp(const WalkGenerator& generator)
{
  if(rcss::Runtime::get().options.motionPolicy != "dancer") return nullptr;
  return std::make_unique<DancerGetUpPhase>(generator);
}
