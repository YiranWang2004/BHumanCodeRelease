#include "DancerKickGate.h"
#include <iostream>
#include <stdexcept>
#include <limits>
using Gate = rcss::DancerKickGate;
static void check(bool condition, const char* why) { if(!condition) throw std::runtime_error(why); }
struct Fixture
{
  Gate gate;
  Gate::Input in;
  Fixture()
  {
    in.entryRequested = in.teamAllowed = in.detected = in.ballUsable = true;
    in.visualRange = in.ballX = in.fieldBallX = 3.;
  }
  bool tick(unsigned now) { in.now = now; return gate.update(in); }
  void enter()
  {
    check(!tick(0) && gate.zeroVelocity, "entry begins with zero AMP");
    check(!tick(280) && gate.zeroVelocity, "entry lasts 300 ms");
    check(tick(300) && gate.reason == "kick", "entry finishes at 300 ms");
  }
  void exiting(unsigned now, const char* why)
  {
    check(tick(now) && gate.lastExitReason == why && gate.stage == Gate::Stage::exitPrepare, why);
    check(tick(now + 120) && gate.zeroVelocity, "Kick retained during exit preparation");
    check(!tick(now + 140) && gate.zeroVelocity, "return to zero AMP at 140 ms");
    in.entryRequested = in.teamAllowed = in.detected = in.ballUsable = true;
    in.visualRange = in.ballX = 3.; in.ballY = 0.;
    check(!tick(now + 1320) && gate.reason == "cooldown", "cooldown measured from actual Walk");
    check(!tick(now + 1340) && gate.reason == "entry_settle", "new entry after 1200 ms cooldown");
    check(tick(now + 1640), "second session settles again");
  }
};
int main()
{
  {
    Fixture f;
    f.in.detected = false; check(!f.tick(0) && f.gate.reason == "waiting_for_vision", "vision entry gate");
    f.in.detected = true; f.in.visualRange = 6.001;
    check(!f.tick(0) && f.gate.reason == "approach", "range entry gate");
    f.in.visualRange = 6.; f.in.bearing = .651;
    check(!f.tick(0) && f.gate.reason == "face_ball", "yaw entry gate");
    f.in.bearing = -.65; f.enter();
    // Equivalent to native branches replacing walkToBallAndKick with walk/stand.
    // Neither branch flags nor native request persistence are session conditions.
    f.in.entryRequested = false;
    check(f.tick(320) && f.gate.reason == "kick", "native walk request must not cancel session");
    f.in.detected = false;
    check(f.tick(340), "missing See frame does not immediately cancel active Kick");
  }
  {
    Fixture f; check(!f.tick(0), "settle"); f.in.entryRequested = false;
    check(f.tick(300), "initial request need not persist while settling");
  }
  for(int change = 0; change < 3; ++change)
  {
    Fixture f; f.tick(0);
    if(change == 0) f.in.detected = false;
    if(change == 1) f.in.teamAllowed = false;
    if(change == 2) f.in.bearing = .66;
    check(!f.tick(100) && f.gate.lastExitReason == "entry_cancelled", "settle abort");
  }
  {
    Fixture f; f.enter(); f.in.teamAllowed = false;
    f.exiting(320, "team_permission_revoked");
  }
  {
    Fixture f; f.enter(); f.in.ballX = -.2; f.in.ballY = .3;
    check(f.tick(880), "bad pose confirmation"); f.exiting(900, "bad_ball_pose");
  }
  {
    Fixture f; f.enter(); f.in.ballX = 9.;
    check(f.tick(1800), "range must wait strictly beyond minimum 1500 ms");
    f.exiting(1820, "ball_out_of_range");
  }
  {
    Fixture f; f.enter(); f.in.ballX = 9.; check(f.tick(700), "range excursion");
    f.in.ballX = 8.; check(f.tick(1600), "range timer resets inside");
    f.in.ballX = 9.; check(f.tick(2180), "new excursion under 600 ms");
    f.exiting(2200, "ball_out_of_range");
  }
  {
    Fixture f; f.enter(); f.in.ballUsable = false;
    check(f.tick(1280), "unusable ball gets 1000 ms confirmation");
    f.exiting(1300, "lost_vision");
  }
  {
    Fixture f; f.enter(); check(f.tick(5280), "no progress under 5000 ms");
    f.exiting(5300, "no_progress");
  }
  for(int progress = 0; progress < 4; ++progress)
  {
    Fixture f; f.enter();
    if(progress == 0) f.in.ballX -= .13;
    if(progress == 1) f.in.fieldBallX += .16;
    if(progress == 2) f.in.robotX += .21;
    if(progress == 3) f.in.robotYaw += .26;
    check(f.tick(5280) && f.tick(5300), "all four progress measures restart timer");
  }
  {
    Fixture f; f.enter();
    for(unsigned t = 320; t < 14300; t += 20)
    {
      f.in.robotX = t * .001; // ongoing useful repositioning, never stall
      check(f.tick(t) && f.gate.reason == "kick", "one extra 7 s round");
    }
    f.exiting(14300, "session_timeout");
  }
  {
    Fixture f; f.enter(); f.in.hardStop = true;
    check(!f.tick(320) && f.gate.lastExitReason == "referee_stop", "referee stop is immediate");
    check(!f.tick(330) && !f.gate.zeroVelocity, "Kick prohibition must allow legal defensive walking");
    f.in.hardStop = false;
    check(!f.tick(340) && f.gate.reason == "cooldown", "referee stop preserves cooldown");
  }
  {
    Fixture f; f.enter(); f.gate.interrupt(320, "motion_interrupted");
    check(!f.tick(1500) && f.gate.reason == "cooldown", "fall/get-up interruption cooldown");
    f.gate.reset(); check(!f.tick(1520) && f.gate.reason == "entry_settle", "teleport resets session");
  }
  std::cout << "Dancer settling, latched session, exits, progress, referee and cooldown passed\n";
}
