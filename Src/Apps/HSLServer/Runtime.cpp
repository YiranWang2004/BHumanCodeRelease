#include "Runtime.h"
#include "Math/Rotation.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdlib>

namespace hsl
{
thread_local unsigned sensorTimestamp = 0;
Runtime& Runtime::get() { static Runtime runtime; return runtime; }
const std::array<const char*, 22>& sensorNames()
{
  static const std::array<const char*, 22> names = {
    "q_hj1", "q_hj2", "q_laj1", "q_laj2", "q_laj3", "q_laj4",
    "q_raj1", "q_raj2", "q_raj3", "q_raj4", "q_llj1", "q_llj2", "q_llj3",
    "q_llj4", "q_llj5", "q_llj6", "q_rlj1", "q_rlj2", "q_rlj3", "q_rlj4", "q_rlj5", "q_rlj6"};
  return names;
}
Options arguments(int argc, char** argv)
{
  Options options;
  const auto numeric = [](const std::string& value)
  {
    size_t end = 0;
    const auto result = std::stod(value, &end);
    if(end != value.size() || !std::isfinite(result)) throw std::runtime_error("Invalid numeric argument: " + value);
    return result;
  };
  for(int i = 1; i < argc; ++i)
  {
    const std::string key = argv[i];
    if(key == "--help" || key == "-h")
    {
      std::cout << "B-Human K1 / dancer-rcssservermj\n"
                << "  --host HOST --port PORT --team NAME --team-id N --team-port UDP_PORT\n"
                << "  --num N --num-players N --role goal_keeper|striker\n"
                << "    hsl_l supports up to 11 players; rosters above 7 use native s11v11\n"
                << "  --field hsl_s|hsl_m|hsl_l|hsl_whrg\n"
                << "  --localization truth|measured (default measured) --sim-speed FACTOR\n"
                << "  --assets-dir PATH --beam-x X --beam-y Y --beam-theta DEG\n"
                << "  --half-seconds N (default 1200) --trace FILE --max-frames N (diagnostics)\n"
                << "  --test-stand | --test-cmd VX,VY,WZ (native motion diagnostics, SI units)\n";
      std::cout << "  --kick-policy native|zjudancer --dancer-repo PATH --dancer-python PYTHON\n";
      std::cout << "  --motion-subframes 1|10 (native Motion scheduling, default 10)\n";
      std::exit(0);
    }
    if(key == "--test-stand") { options.debugStand = true; continue; }
    if(i + 1 == argc) throw std::runtime_error("Missing value for " + key);
    const std::string value = argv[++i];
    const auto integer = [&]
    { const double n = numeric(value); if(n != std::floor(n) || n < 0 || n > 65535) throw std::runtime_error("Invalid integer for " + key); return static_cast<int>(n); };
    if(key == "--host") options.host = value;
    else if(key == "--port") options.port = integer();
    else if(key == "--team") options.team = value;
    else if(key == "--team-port") options.teamPort = integer();
    else if(key == "--team-id") options.teamId = integer();
    else if(key == "--num") options.number = integer();
    else if(key == "--num-players") options.players = integer();
    else if(key == "--role") options.role = value;
    else if(key == "--field") options.field = value;
    else if(key == "--localization") options.localization = value;
    else if(key == "--sim-speed") options.speed = numeric(value);
    else if(key == "--assets-dir") options.assets = value;
    else if(key == "--kick-policy") options.kickPolicy = value;
    else if(key == "--dancer-repo") options.dancerRepo = value;
    else if(key == "--dancer-python") options.dancerPython = value;
    else if(key == "--trace") options.trace = value;
    else if(key == "--half-seconds") options.halfSeconds = integer();
    else if(key == "--max-frames") options.maxFrames = integer();
    else if(key == "--motion-subframes") options.motionSubframes = integer();
    else if(key == "--beam-x") { options.beamX = numeric(value); options.beamExplicit = true; }
    else if(key == "--beam-y") { options.beamY = numeric(value); options.beamExplicit = true; }
    else if(key == "--beam-theta") options.beamYaw = numeric(value);
    else if(key == "--test-cmd")
    {
      std::string values = value;
      std::replace(values.begin(), values.end(), ',', ' ');
      std::istringstream input(values);
      float x, y, yaw;
      if(!(input >> x >> y >> yaw) || !(input >> std::ws).eof() || !std::isfinite(x + y + yaw)) throw std::runtime_error("--test-cmd requires VX,VY,WZ");
      options.debugWalk = true; options.debugVelocity = Pose2f(yaw, x * 1000.f, y * 1000.f);
    }
    else throw std::runtime_error("Unknown option: " + key);
  }
  if(options.kickPolicy != "native" && options.kickPolicy != "zjudancer") throw std::runtime_error("--kick-policy must be native or zjudancer");
  if(options.kickPolicy == "zjudancer" && options.dancerRepo.empty()) throw std::runtime_error("--dancer-repo is required for zjudancer kick policy");
  if(options.localization != "truth" && options.localization != "measured") throw std::runtime_error("--localization must be truth or measured");
  if(options.halfSeconds < 1) throw std::runtime_error("--half-seconds must be positive");
  if(options.motionSubframes != 1 && options.motionSubframes != 10) throw std::runtime_error("--motion-subframes must be 1 or 10");
  if(options.field != "hsl_s" && options.field != "hsl_m" && options.field != "hsl_l" && options.field != "hsl_whrg") throw std::runtime_error("Unknown HSL field");
  const int maxPlayers = options.field == "hsl_l" ? 11 : 7;
  if(options.players < 1 || options.players > maxPlayers || options.number < 1 || options.number > options.players || options.teamId < 1 || options.teamId > 99 || options.port < 1 || options.speed <= 0.) throw std::runtime_error("Invalid roster, port, team id or simulation speed (hsl_l: 1-11 players; other fields: 1-7)");
  if(options.team.empty() || options.team.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) throw std::runtime_error("Team name must contain only ASCII letters, digits, _ or -");
  if(!options.role.empty() && options.role != (options.number == 1 ? "goal_keeper" : "striker")) throw std::runtime_error("B-Human uses player 1 as goalkeeper and other players as field players");
  return options;
}
void Runtime::stop()
{
  std::lock_guard<std::mutex> lock(mutex);
  running = false; changed.notify_all();
}
Vector3a Runtime::tilt() const
{
  Quaternionf q(static_cast<float>(frame.quaternion[0]), static_cast<float>(frame.quaternion[1]), static_cast<float>(frame.quaternion[2]), static_cast<float>(frame.quaternion[3]));
  if(q.norm() < .5f) return Vector3a::Zero();
  q.normalize();
  const auto matrix = q.toRotationMatrix();
  // Only the gravity-relative roll/pitch enter the IMU adapter.
  return Vector3a(std::atan2(matrix(2, 1), matrix(2, 2)), std::asin(std::clamp(-matrix(2, 0), -1.f, 1.f)), 0.f);
}
Pose2f Runtime::truth() const
{
  const auto& q = frame.quaternion;
  const float yaw = static_cast<float>(std::atan2(2. * (q[0] * q[3] + q[1] * q[2]), 1. - 2. * (q[2] * q[2] + q[3] * q[3])));
  Pose2f result(yaw, static_cast<float>(frame.position[0] * 1000.), static_cast<float>(frame.position[1] * 1000.));
  if(!left) { result.translation = -result.translation; result.rotation = Angle::normalize(result.rotation + Constants::pi); }
  return result;
}
std::string Runtime::action() const
{
  static const std::array<const char*, 22> actuators = {
    "he1", "he2", "lae1", "lae2", "lae3", "lae4", "rae1", "rae2", "rae3", "rae4",
    "lle1", "lle2", "lle3", "lle4", "lle5", "lle6", "rle1", "rle2", "rle3", "rle4", "rle5", "rle6"};
  std::ostringstream result;
  result << std::setprecision(8);
  for(size_t i = 0; i < 22; ++i)
  {
    const auto joint = motors.jointMapping.at(i);
    const auto& gains = motorParameters.controlParameters[request.jointStiffnessMapping][joint];
    const float desired = request.angles[joint];
    const float stiffness = desired == JointAngles::off || desired == JointAngles::ignore ? 0.f : std::clamp(request.stiffnessData.stiffnesses[joint] * .01f, 0.f, 1.f);
    const float target = stiffness > 0.f ? desired + static_cast<float>(calibration.offsets[joint]) : static_cast<float>(measurements.angles[joint]);
    // Match SimulatedRobot3D: native 2026 requests use the selected PD gains.
    const float torque = 0.f;
    if(!std::isfinite(target + torque)) throw std::runtime_error("Non-finite B-Human joint command");
    result << '(' << actuators[i] << ' ' << target * 180.f / Constants::pi << " 0 " << gains.p * stiffness << ' ' << gains.d * stiffness << ' ' << torque << ' ' << gains.maxTorque << ')';
  }
  return result.str();
}
}

void bhumanHSLPolicyInference(const char* model)
{
  auto& runtime = hsl::Runtime::get();
  runtime.policy = model;
  runtime.policyTime = hsl::sensorTimestamp;
  if(runtime.policy == "K1_walk.onnx") ++runtime.walkInferences;
  else ++runtime.kickInferences;
}

// Only the selected HSL team substitutes the native walk/stand actor.
bool bhumanHSLUsesAmpWalk()
{
  return hsl::Runtime::get().options.kickPolicy == "zjudancer";
}
