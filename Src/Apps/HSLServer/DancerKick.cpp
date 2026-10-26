#include "DancerKick.h"
#include "Runtime.h"
#include "Representations/Infrastructure/GameState.h"
#include "../../../Util/BoosterRoboticsSDK/include/booster/third_party/nlohmann_json/json.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <iomanip>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hsl
{
namespace
{
using Json = nlohmann::json;
class Worker
{
  int socket = -1;
  pid_t pid = -1;
  std::string input;
public:
  Worker(const Options& options)
  {
    const auto slot = options.host + ":" + std::to_string(options.port) + ":" + options.team;
    const auto script = options.assets + "/scripts/dancer_kick_worker.py";
    int sockets[2];
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) throw std::runtime_error("Cannot create kick worker socket");
    pid = fork();
    if(pid < 0) { close(sockets[0]); close(sockets[1]); throw std::runtime_error("Cannot fork kick worker"); }
    if(pid == 0)
    {
      close(sockets[0]);
      if(sockets[1] != 3) { if(dup2(sockets[1], 3) < 0) _exit(127); close(sockets[1]); }
      execlp(options.dancerPython.c_str(), options.dancerPython.c_str(), script.c_str(),
             "--fd", "3", "--dancer-repo", options.dancerRepo.c_str(), "--slot-key", slot.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    socket = sockets[0]; close(sockets[1]);
    try { if(!exchange("", 45000).at("loaded").get<bool>()) throw std::runtime_error("Kick worker not loaded"); }
    catch(...) { stop(); throw; }
  }
  ~Worker() { stop(); }
  void stop()
  {
    if(socket >= 0) { close(socket); socket = -1; }
    if(pid > 0)
    {
      for(int i = 0; i < 100; ++i)
      { if(waitpid(pid, nullptr, WNOHANG) == pid) { pid = -1; return; } usleep(10000); }
      kill(pid, SIGTERM); waitpid(pid, nullptr, 0); pid = -1;
    }
  }
  Json exchange(const std::string& request, int timeout = 2000)
  {
    if(!request.empty())
    {
      const auto wire = request + '\n';
      for(size_t sent = 0; sent < wire.size();)
      {
        const auto n = send(socket, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
        if(n <= 0) throw std::runtime_error("Kick worker disconnected");
        sent += static_cast<size_t>(n);
      }
    }
    while(true)
    {
      const auto end = input.find('\n');
      if(end != std::string::npos)
      {
        const auto line = input.substr(0, end); input.erase(0, end + 1);
        auto reply = Json::parse(line);
        if(reply.contains("error")) throw std::runtime_error("Kick worker: " + reply.at("error").get<std::string>());
        return reply;
      }
      pollfd fd{socket, POLLIN, 0};
      if(poll(&fd, 1, timeout) <= 0) throw std::runtime_error("Kick worker timeout");
      char buffer[8192]; const auto n = recv(socket, buffer, sizeof(buffer), 0);
      if(n <= 0) throw std::runtime_error("Kick worker exited; check Python/model dependencies");
      input.append(buffer, static_cast<size_t>(n));
      if(input.size() > 1048576) throw std::runtime_error("Oversized kick worker response");
    }
  }
};
std::unique_ptr<Worker> worker;
}
void startDancerKick()
{
  auto& options = Runtime::get().options;
  if(options.kickPolicy != "zjudancer") return;
  options.dancerRepo = std::filesystem::absolute(options.dancerRepo).string();
  worker = std::make_unique<Worker>(options);
}
void stepDancerKick(const MotionRequest& request)
{
  auto& r = Runtime::get();
  std::array<double, 22> q{}, dq{};
  for(size_t i = 0; i < sensorNames().size(); ++i)
  {
    const auto joint = std::find_if(r.frame.joints.begin(), r.frame.joints.end(),
      [&](const auto& j) { return j.name == sensorNames()[i]; });
    if(joint == r.frame.joints.end()) throw std::runtime_error("Missing kick sensor joint");
    q[i] = joint->q; dq[i] = joint->dq;
  }
  const auto tilt = r.tilt();
  const auto pose = r.estimate;
  const Vector2f goal = pose.inverse() * Vector2f(r.goalX * 1000.f, 0.f);
  const Vector2f ballField = pose * r.ball.estimate.position;
  const auto& b = r.ball.estimate.position;
  const auto& v = r.ball.estimate.velocity;
  const bool intent = request.motion == MotionRequest::walkToBallAndKick || request.motion == MotionRequest::dribble;
  std::array<double, 2> head{q[0], q[1]};
  for(size_t i = 0; i < 2; ++i)
  {
    const auto joint = r.motors.jointMapping[i];
    const double angle = r.request.angles[joint];
    if(std::isfinite(angle) && std::abs(angle) < 3.) head[i] = angle + static_cast<float>(r.calibration.offsets[joint]);
  }
  const bool locomotion = (r.motionInfo.executedPhase == MotionPhase::walk || r.motionInfo.executedPhase == MotionPhase::stand) &&
    (request.isWalking() || request.motion == MotionRequest::stand);
  const bool opponentKickoff = r.frame.mode.rfind("KickOff", 0) == 0 &&
    ((r.frame.mode.find("Left") != std::string::npos) != r.left);
  const bool hold = r.frame.penalized || r.frame.stopped || opponentKickoff ||
    r.gameState == GameState::opponentKickOff || request.motion == MotionRequest::stand;
  const auto speed = hold ? Pose2f() : r.motionInfo.speed;
  const std::string block = r.frame.penalized ? "penalized" : r.frame.stopped ? "server stopped" :
    !locomotion ? "native recovery/prepare" : !r.kickAllowed ? "game state forbids kick" :
    (r.options.debugStand || r.options.debugWalk) ? "diagnostic mode" : "";
  Json packet = {{"time", r.frame.time}, {"number", r.options.number},
    {"play_time", r.frame.playTime}, {"mode", r.frame.mode}, {"block_reason", block},
    {"pose", {pose.translation.x()*.001, pose.translation.y()*.001, static_cast<float>(pose.rotation)}},
    {"field", {r.goalX, r.fieldHalfWidth}},
    {"goalie_area", ballField.x()*.001 >= -r.goalX && ballField.x()*.001 <= r.goalieAreaX && std::abs(ballField.y()*.001) <= r.goalieAreaY},
    {"loco_enabled", locomotion},
    {"velocity", {speed.translation.x() * .001, speed.translation.y() * .001, static_cast<float>(speed.rotation)}},
    {"allowed", locomotion && r.kickAllowed && !r.frame.penalized && !r.frame.stopped && !r.options.debugStand && !r.options.debugWalk},
    {"set_play_kicker", r.ownSetPlayKicker}, {"head", head}, {"owner", r.ballOwner || intent}, {"intent", intent},
    {"ball", {b.x() * .001, b.y() * .001}}, {"ball_velocity", {v.x() * .001, v.y() * .001}},
    {"ball_field", {ballField.x() * .001, ballField.y() * .001}},
    {"ball_stamp", (static_cast<double>(r.visualBallTime) - 100000.) * .001},
    {"seen", r.visualDetected}, {"vision", r.frame.hasVision},
    {"vision_fresh", r.timestamp - r.visionTime <= 1000},
    {"visual_ball", {r.visualBall.x(), r.visualBall.y()}},
    {"goal", {goal.x() * .001, goal.y() * .001}},
    {"ball_out", std::abs(ballField.x() * .001) > r.goalX + .1 || std::abs(ballField.y() * .001) > r.fieldHalfWidth + .1},
    {"state", {{"q", q}, {"dq", dq}, {"gyro", r.frame.gyro},
               {"rpy", {static_cast<float>(tilt.x()), static_cast<float>(tilt.y()), static_cast<float>(pose.rotation)}}}}};
  const auto result = worker->exchange(packet.dump());
  r.ampInferences = result.value("amp_inferences", r.ampInferences);
  r.dancerInferences = result.value("inferences", r.dancerInferences);
  r.dancerHolding = result.at("holding").get<bool>();
  const auto phase = result.at("phase").get<std::string>();
  if(phase != r.dancerPhase)
  {
    std::cerr << "[DancerKick] t=" << r.frame.time << " phase=" << phase << " reason=" << result.value("reason", "") << '\n';
    r.dancerPhase = phase;
  }
  r.dancerAction.clear();
  if(!result.contains("q")) return;
  static const char* names[] = {"he1","he2","lae1","lae2","lae3","lae4","rae1","rae2","rae3","rae4",
    "lle1","lle2","lle3","lle4","lle5","lle6","rle1","rle2","rle3","rle4","rle5","rle6"};
  std::ostringstream action; action << std::setprecision(9);
  for(size_t i = 0; i < 22; ++i)
  {
    const double target = result.at("q").at(i), kp = result.at("kp").at(i), kd = result.at("kd").at(i);
    if(!std::isfinite(target + kp + kd) || kp < 0 || kd < 0) throw std::runtime_error("Invalid kick motor target");
    action << '(' << names[i] << ' ' << target * 180. / Constants::pi << " 0 " << kp << ' ' << kd << " 0)";
  }
  r.dancerAction = action.str();
}
}
