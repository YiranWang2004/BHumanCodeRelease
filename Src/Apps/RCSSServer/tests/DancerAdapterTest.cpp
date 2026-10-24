#include "Runtime.h"
#include "Platform/SystemCall.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
SystemCall::Mode SystemCall::getMode() { return simulatedRobot; }
extern void bhumanRCSSDancerMotionInterrupted();
extern float bhumanRCSSDancerForwardSpeed();
extern bool bhumanRCSSDancerStep(bool, bool, const Pose2f&, float, const Vector2f&, unsigned, JointAngles&, bool&);
static void check(bool passed, const char* why) { if(!passed) throw std::runtime_error(why); }
int main()
{
  auto& r = rcss::Runtime::get();
  r.options.motionPolicy = "dancer"; r.options.serverModel = "K1";
  check(bhumanRCSSDancerForwardSpeed() == 2500.f, "standard K1 planner shares AMP forward ceiling");
  r.options.motionPolicy = "native";
  check(bhumanRCSSDancerForwardSpeed() == 0.f, "native planner must retain configured speeds");
  r.options.motionPolicy = "dancer"; r.options.serverModel = "K1BHuman";
  check(bhumanRCSSDancerForwardSpeed() == 0.f, "untested K1BHuman retains configured speeds");
  r.options.serverModel = "K1";
  r.dancerBallOwner = r.dancerGameAllowed = true;
  r.frame.position = {0., 0., .55};
  for(const auto& name : rcss::sensorNames()) r.frame.joints.push_back({name});
  for(int j = 0; j < 22; ++j) r.motors.jointMapping.push_back(static_cast<Joints::Joint>(j));
  r.frame.objects.push_back({"B", 3., 0., 0.});
  JointAngles target;
  bool kicking = false;
  const auto tick = [&](unsigned ms, bool entry, bool standing, unsigned flags)
  {
    ++r.sequence; r.timestamp = 100000 + ms; rcss::sensorTimestamp = r.timestamp;
    r.frame.time = ms * .001;
    r.frame.hasVision = ms % 40 == 0;
    check(bhumanRCSSDancerStep(entry, standing, Pose2f(.2f, 800.f, 100.f), entry ? 0.f : .9f,
                               Vector2f(3000.f, 0.f), flags, target, kicking), "adapter active");
    for(size_t j = 2; j < 22; ++j) check(std::isfinite(target.angles[j]), "finite joint target");
  };
  for(unsigned ms = 0; ms < 300; ms += 20)
  {
    tick(ms, ms == 0, false, 0);
    check(!kicking && r.dancerGate == "entry_settle", "40 ms See / 20 ms motion must finish settling");
    check(r.dancerVelocity == std::array<double, 3>{}, "AMP settling must use zero command");
  }
  tick(300, false, false, 0);
  check(kicking && r.kickInferences == 1, "first Kick after exactly 300 ms");
  for(unsigned flag : {1u, 2u, 4u, 8u, 16u, 31u})
  {
    const auto before = r.kickInferences;
    tick(300 + flag * 20, false, true, flag);
    check(kicking && r.dancerGate == "kick" && r.kickInferences == before + 1, "native flag/stand cannot interrupt session");
    check(std::abs(r.dancerKickDirectionWorld) < 1.e-6, "non-ball request must not overwrite tactical direction");
    // A proposed phase can call the adapter twice within the same frame.
    bhumanRCSSDancerStep(false, true, Pose2f(), 0.f, Vector2f(3000.f, 0.f), flag, target, kicking);
    check(r.kickInferences == before + 1, "one inference per network frame");
  }
  r.dancerBallOwner = false;
  tick(1000, false, true, 31);
  check(kicking && r.dancerGate == "exit_prepare:team_permission_revoked", "team permission begins controlled exit");
  tick(1120, false, true, 31); check(kicking, "Kick remains in exit preparation");
  tick(1140, false, false, 0);
  check(!kicking && r.dancerGate == "cooldown" && r.dancerVelocity == std::array<double, 3>{}, "zero AMP at exit");
  r.dancerBallOwner = true;
  tick(2320, true, false, 0); check(!kicking && r.dancerGate == "cooldown", "cooldown from actual Walk");
  tick(2360, true, false, 0); check(!kicking && r.dancerGate == "entry_settle", "can start again after cooldown");
  // Continuous See heartbeat while settling again.
  for(unsigned ms = 2380; ms <= 2660; ms += 20) tick(ms, true, false, 0);
  check(kicking, "second Kick");
  r.dancerForcedStop = true;
  tick(2680, true, false, 0);
  check(!kicking && r.dancerExitReason == "referee_stop", "referee stop immediately overrides latched session");
  tick(2700, false, false, 0);
  check(!kicking && r.dancerVelocity[0] > .7, "Kick denied must not freeze legal defensive AMP walking");
  r.dancerForcedStop = false;
  for(unsigned ms = 3880; ms <= 4180; ms += 20) tick(ms, true, false, 0);
  check(kicking, "third session before fall interruption");
  bhumanRCSSDancerMotionInterrupted();
  check(r.dancerExitReason == "motion_interrupted", "fall/get-up recorded immediately");
  tick(4200, true, false, 0);
  check(!kicking && r.dancerGate == "cooldown", "fall interruption is latched");
  std::cout << "Real AMP/VisualKick adapter: cadence, zero settle, all native flags, ownership and referee passed\n";
}
