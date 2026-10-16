#include "SensorProvider.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

MAKE_MODULE(RCSSSensorProvider);

void RCSSSensorProvider::update(CameraInfo& data)
{
  data.width = 544; data.height = 448;
  data.openingAngleWidth = 105_deg; data.openingAngleHeight = 94_deg;
  data.opticalCenter = Vector2f(272.f, 224.f);
  data.updateFocalLength();
}


void RCSSSensorProvider::update(FrameInfo& data) { data.time = rcss::sensorTimestamp; }
void RCSSSensorProvider::update(JointSensorData& data)
{
  const auto& runtime = rcss::Runtime::get();
  const auto& names = rcss::sensorNames();
  data.timestamp = rcss::sensorTimestamp;
  data.angles.fill(0.f); data.velocity.fill(0.f); data.torque.fill(0.f);
  data.temperatures.fill(30); data.status.fill(JointSensorData::regular);
  for(const auto& measurement : runtime.frame.joints)
  {
    const auto found = std::find(names.begin(), names.end(), measurement.name);
    if(found == names.end()) continue;
    if(!measurement.hasTorque) throw std::runtime_error("Server lacks HJ tau: restart the updated rcssservermj");
    const auto joint = runtime.motors.jointMapping.at(static_cast<size_t>(found - names.begin()));
    data.angles[joint] = static_cast<float>(measurement.q);
    data.velocity[joint] = static_cast<float>(measurement.dq);
    data.torque[joint] = static_cast<float>(measurement.tau);
  }
}
void RCSSSensorProvider::update(InertialSensorData& data)
{
  const auto& runtime = rcss::Runtime::get();
  const auto tilt = runtime.tilt();
  const double sensorTime = rcss::sensorTimestamp * .001;
  const double dt = lastIMUTime < 0. ? 0. : std::clamp(sensorTime - lastIMUTime, 0., .1);
  lastIMUTime = sensorTime;
  data.angle = orientation.update(Vector3f(static_cast<float>(runtime.frame.gyro[0]), static_cast<float>(runtime.frame.gyro[1]), static_cast<float>(runtime.frame.gyro[2])), tilt, static_cast<float>(dt));
  for(size_t i = 0; i < 3; ++i)
  {
    data.gyro(static_cast<int>(i)) = static_cast<float>(runtime.frame.gyro[i]);
    data.acc(static_cast<int>(i)) = static_cast<float>(runtime.frame.acceleration[i]);
  }
}
void RCSSSensorProvider::update(SystemSensorData& data) { data.batteryLevel = 1.f; }
void RCSSSensorProvider::update(KeyStates& data) { data.pressed.fill(false); }
void RCSSSensorProvider::update(RobotPose& data)
{
  const auto& runtime = rcss::Runtime::get();
  if(runtime.options.localization != "truth") throw std::runtime_error("Truth pose provider selected in measured mode");
  data = runtime.truth(); data.quality = RobotPose::superb;
}
void RCSSSensorProvider::update(GameControllerData& data)
{
  const auto& r = rcss::Runtime::get();
  const auto& frame = r.frame;
  data = GameControllerData();
  // Native GameControllerRBS expects a common broadcast sequence (~2 Hz),
  // independent of when each player connected. Use the shared server clock.
  const unsigned gcTick = static_cast<unsigned>(std::floor(frame.time * 2.));
  data.timeLastPacketReceived = 100000u + gcTick * 500u;
  data.isTrueData = true;
  data.packetNumber = static_cast<uint8_t>(gcTick);
  data.playersPerTeam = static_cast<uint8_t>(r.options.players);
  data.competitionType = COMPETITION_TYPE_LARGE;
  data.gamePhase = frame.phase == 4 ? GAME_PHASE_PENALTY_SHOOT_OUT : frame.phase >= 2 ? GAME_PHASE_EXTRA_TIME : GAME_PHASE_NORMAL;
  data.firstHalf = frame.phase % 2 == 0;
  const double duration = frame.phase < 2 ? 300. : 180.;
  const double endTime = frame.phase < 2 ? (frame.phase + 1) * 300. : frame.phase < 4 ? 600. + (frame.phase - 1) * 180. : 0.;
  data.secsRemaining = static_cast<int16_t>(std::clamp(endTime - frame.playTime, 0., duration));
  data.setPlay = SET_PLAY_NONE;
  data.state = frame.mode == "Initial" || frame.mode == "BeforeKickOff" || frame.mode.empty() ? STATE_INITIAL :
               frame.mode.rfind("Ready", 0) == 0 ? STATE_READY :
               frame.mode.rfind("Goal_", 0) == 0 ? STATE_INITIAL :
               frame.mode == "GameOver" || frame.mode == "Finished" || frame.mode == "HalfTime" ? STATE_FINISHED : STATE_PLAYING;
  const int own = r.left ? 0 : 1, other = 1 - own;
  data.teams[own].teamNumber = static_cast<uint8_t>(r.options.teamId);
  data.teams[other].teamNumber = static_cast<uint8_t>(r.options.teamId == 1 ? 2 : 1);
  for(int team = 0; team < 2; ++team)
  {
    auto& info = data.teams[team];
    info.fieldPlayerColor = info.goalkeeperColor = static_cast<uint8_t>(team == 0 ? TEAM_BLUE : TEAM_RED);
    info.goalkeeper = 1;
    info.score = static_cast<uint8_t>(team == 0 ? frame.leftScore : frame.rightScore);
    info.messageBudget = 12000; // Native Code Release 2026 total match budget.
    for(size_t i = 0; i < MAX_NUM_PLAYERS; ++i)
      info.players[i].penalty = i < static_cast<size_t>(r.options.players) ? PENALTY_NONE : PENALTY_SUBSTITUTE;
  }
  auto& player = data.teams[own].players[r.options.number - 1];
  player.penalty = frame.penalized ? PENALTY_ILLEGAL_POSITIONING : PENALTY_NONE;
  player.secsTillUnpenalised = static_cast<uint8_t>(std::clamp(frame.penaltySeconds, 0., 255.));
  int kicking = frame.kickoffTeam;
  if(frame.mode.find("Left") != std::string::npos || frame.mode.ends_with("_left")) kicking = 0;
  if(frame.mode.find("Right") != std::string::npos || frame.mode.ends_with("_right")) kicking = 1;
  if(data.state == STATE_INITIAL || data.state == STATE_READY || data.state == STATE_FINISHED)
  { kickoffStarted = -1.; kickoffTeam = -1; }
  if(frame.mode.rfind("KickOff", 0) == 0 && (kickoffStarted < 0. || frame.mode != previousMode))
  { kickoffStarted = frame.time; kickoffTeam = kicking; }
  // RCSS frees a kick-off after 15 s or a touch. Preserve B-Human's native
  // ball-movement check during that window, including just after the first
  // touch switches the server to PlayOn.
  if(kickoffStarted >= 0. && (frame.mode.rfind("KickOff", 0) == 0 || frame.mode == "PlayOn"))
  {
    data.secondaryTime = static_cast<int16_t>(std::ceil(std::clamp(15. - (frame.time - kickoffStarted), 0., 15.)));
    kicking = kickoffTeam;
  }
  // This server has no Ready/Set phases. Present a brief preparation window
  // for the native Behavior to select and initialize its 7v7 kick-off play.
  // The server's own 15 s kickoff timer continues throughout this window.
  if(frame.mode.rfind("KickOff", 0) == 0)
    data.state = frame.time - kickoffStarted < .6 ? STATE_READY : frame.time - kickoffStarted < 1.2 ? STATE_SET : STATE_PLAYING;
  previousMode = frame.mode;
  data.kickingTeam = kicking < 0 ? KICKING_TEAM_NONE : data.teams[kicking].teamNumber;
  if(frame.mode == "Goal_Left") data.kickingTeam = data.teams[1].teamNumber;
  if(frame.mode == "Goal_Right") data.kickingTeam = data.teams[0].teamNumber;
  if(frame.mode.rfind("goal_kick", 0) == 0) data.setPlay = SET_PLAY_GOAL_KICK;
  else if(frame.mode.rfind("corner_kick", 0) == 0) data.setPlay = SET_PLAY_CORNER_KICK;
  else if(frame.mode.rfind("KickIn", 0) == 0) data.setPlay = SET_PLAY_THROW_IN;
  else if(frame.mode.rfind("penalty_kick", 0) == 0 || frame.mode.rfind("penalty_shoot", 0) == 0) data.setPlay = SET_PLAY_PENALTY_KICK;
  else if(frame.mode.find("free_kick") != std::string::npos || frame.mode.rfind("offside", 0) == 0) data.setPlay = SET_PLAY_INDIRECT_FREE_KICK;
}
