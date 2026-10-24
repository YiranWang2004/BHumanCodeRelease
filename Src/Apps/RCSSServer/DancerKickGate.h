#pragma once
#include <cmath>
#include <string>

namespace rcss
{
/** Simulation RLVisionKick transition rules from dancer-robocupdemo.
 * B-Human supplies team ownership and referee permission separately from its
 * transient walk/interception/avoidance request. All times are simulation ms.
 */
class DancerKickGate
{
public:
  struct Input
  {
    unsigned now = 0;
    bool entryRequested = false, teamAllowed = false, hardStop = false;
    bool detected = false, ballUsable = false;
    double visualRange = 0., bearing = 0.;
    double ballX = 0., ballY = 0., fieldBallX = 0., fieldBallY = 0.;
    double robotX = 0., robotY = 0., robotYaw = 0., kickDirection = 0.;
  };
  enum class Stage { walk, entrySettle, kick, exitPrepare };
  Stage stage = Stage::walk;
  std::string reason = "inactive", lastExitReason = "none";
  bool zeroVelocity = false;

  void reset() { *this = DancerKickGate(); }
  bool session() const { return stage != Stage::walk; }
  void interrupt(unsigned now, const char* why)
  {
    if(session()) { hasExited = true; exited = now; lastExitReason = why; }
    stage = Stage::walk; reason = why; zeroVelocity = true;
  }
  bool update(const Input& in)
  {
    zeroVelocity = false;
    if(in.hardStop)
    {
      const bool wasInSession = session();
      interrupt(in.now, "referee_stop");
      // Denying Kick must not suppress legal AMP movement (e.g. defending an
      // opponent free kick). Only the actual interruption emits zero once.
      zeroVelocity = wasInSession;
      return false;
    }
    if(stage == Stage::exitPrepare)
    {
      zeroVelocity = true;
      if(in.now - transitionStart < 140) return true;
      interrupt(in.now, lastExitReason.c_str());
      reason = "cooldown";
      return false;
    }
    const bool entryReady = in.teamAllowed && in.detected && std::isfinite(in.visualRange) &&
      in.visualRange >= 0. && in.visualRange <= 6. && std::isfinite(in.bearing) && std::abs(in.bearing) <= .65;
    if(stage == Stage::entrySettle)
    {
      zeroVelocity = true;
      if(!entryReady) { interrupt(in.now, "entry_cancelled"); return false; }
      if(in.now - transitionStart < 300) return false;
      stage = Stage::kick;
      started = roundStarted = lastUsable = lastGoodPose = lastInRange = lastProgress = in.now;
      extraRound = false;
      baselineValid = false;
      saveProgress(in);
      reason = "kick";
      return true;
    }
    if(stage == Stage::walk)
    {
      if(hasExited && in.now - exited < 1200) { reason = "cooldown"; return false; }
      if(!in.teamAllowed) { reason = "no_team_permission"; return false; }
      if(!in.entryRequested) { reason = "waiting_for_ball_request"; return false; }
      if(!in.detected) { reason = "waiting_for_vision"; return false; }
      if(!std::isfinite(in.visualRange) || in.visualRange < 0. || in.visualRange > 6.)
      { reason = "approach"; return false; }
      if(!entryReady) { reason = "face_ball"; return false; }
      stage = Stage::entrySettle; transitionStart = in.now;
      zeroVelocity = true; reason = "entry_settle";
      return false;
    }
    const auto leave = [&](const char* why)
    {
      lastExitReason = why; reason = std::string("exit_prepare:") + why;
      stage = Stage::exitPrepare; transitionStart = in.now; zeroVelocity = true;
      // Source sends zero walk velocity here, but VisualKick remains active
      // until robocupWalk after 140 ms. Its fixed kick command is unchanged.
      return true;
    };
    if(!in.teamAllowed) return leave("team_permission_revoked");
    if(in.ballUsable) lastUsable = in.now;
    else if(in.now - lastUsable >= 1000) return leave("lost_vision");
    const double range = std::hypot(in.ballX, in.ballY);
    const bool badPose = std::isfinite(range) && range > .10 &&
      (in.ballX < -.10 || std::abs(std::atan2(in.ballY, in.ballX)) > 1.75);
    if(!badPose) lastGoodPose = in.now;
    if(!std::isfinite(range) || range <= 8.5) lastInRange = in.now;
    if(!baselineValid) saveProgress(in);
    else
    {
      const double forward = (in.fieldBallX - progress.fieldBallX) * std::cos(progress.kickDirection) +
        (in.fieldBallY - progress.fieldBallY) * std::sin(progress.kickDirection);
      const double yawDelta = std::atan2(std::sin(in.robotYaw - progress.robotYaw), std::cos(in.robotYaw - progress.robotYaw));
      if(std::hypot(progress.ballX, progress.ballY) - range >= .12 || forward >= .15 ||
         std::hypot(in.robotX - progress.robotX, in.robotY - progress.robotY) >= .20 || std::abs(yawDelta) >= .25)
        saveProgress(in);
    }
    if(in.now - lastGoodPose >= 600) return leave("bad_ball_pose");
    if(in.now - started >= 14000) return leave("session_timeout");
    if(in.now - lastProgress >= 5000) return leave("no_progress");
    if(in.now - roundStarted > 1500 && in.now - lastInRange >= 600) return leave("ball_out_of_range");
    if(in.now - roundStarted > 7000)
    {
      if(!extraRound && in.ballUsable) { extraRound = true; roundStarted = in.now; }
      else return leave("round_timeout");
    }
    reason = "kick";
    return true;
  }
private:
  bool hasExited = false, baselineValid = false, extraRound = false;
  unsigned exited = 0, transitionStart = 0, started = 0, roundStarted = 0;
  unsigned lastUsable = 0, lastGoodPose = 0, lastInRange = 0, lastProgress = 0;
  Input progress;
  void saveProgress(const Input& in)
  {
    if(!std::isfinite(in.ballX) || !std::isfinite(in.ballY) || !std::isfinite(in.fieldBallX) || !std::isfinite(in.fieldBallY)) return;
    progress = in; baselineValid = true; lastProgress = in.now;
  }
};
}
