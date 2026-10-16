#include "Monitor.h"
#include "Runtime.h"
#include "Streaming/TypeRegistry.h"
#include <algorithm>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace
{
std::string quoted(const std::string& value)
{
  std::ostringstream out;
  out << '"';
  for(unsigned char c : value)
    switch(c)
    {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if(c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else out << c;
    }
  return out.str() + '"';
}
}

namespace rcss
{
std::string monitorSnapshot(const Runtime& r)
{
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(9);
  const auto truth = r.truth();
  const bool currentPolicy = r.policy != "none" && r.policyTime >= r.timestamp && r.policyTime < r.timestamp + 20;
  out << "{\"schema\":\"bhuman-monitor/v1\",\"team\":" << quoted(r.options.team)
      << ",\"server\":" << quoted(r.options.host + ":" + std::to_string(r.options.port))
      << ",\"team_id\":" << r.options.teamId << ",\"player\":" << r.options.number
      << ",\"robot\":" << quoted(r.options.robot) << ",\"localization\":" << quoted(r.options.localization)
      << ",\"session\":" << quoted([] { static const std::string session = std::to_string(getpid()) + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()); return session; }()) << ",\"sequence\":" << r.sequence
      << ",\"time\":" << r.frame.time << ",\"play_time\":" << r.frame.playTime
      << ",\"mode\":" << quoted(r.frame.mode) << ",\"left\":" << (r.left ? "true" : "false")
      << ",\"penalized\":" << (r.frame.penalized ? "true" : "false")
      << ",\"role\":" << quoted(r.role) << ",\"position\":" << quoted(r.position)
      << ",\"skill\":" << quoted(r.skill)
      << ",\"tactic\":" << quoted(r.tactic) << ",\"set_play\":" << quoted(r.setPlay)
      << ",\"motion\":" << quoted(TypeRegistry::getEnumName(r.motion.motion))
      << ",\"phase\":" << quoted(TypeRegistry::getEnumName(r.motionInfo.executedPhase))
      << ",\"policy\":" << quoted(r.policy) << ",\"policy_current\":" << (currentPolicy ? "true" : "false")
      << ",\"policy_kind\":" << quoted(r.policyKind)
      << ",\"policy_age\":" << (r.policyTime ? std::max(0., (static_cast<double>(r.timestamp) - r.policyTime) / 1000.) : -1.)
      << ",\"walk_inferences\":" << r.walkInferences << ",\"kick_inferences\":" << r.kickInferences
      << ",\"get_up_inferences\":" << r.getUpInferences
      << ",\"ball_age\":" << (r.timestamp - r.ball.timeWhenLastSeen) * .001
      << ",\"ball_x\":" << r.ball.estimate.position.x() / 1000.f << ",\"ball_y\":" << r.ball.estimate.position.y() / 1000.f
      << ",\"x\":" << r.estimate.translation.x() / 1000.f << ",\"y\":" << r.estimate.translation.y() / 1000.f
      << ",\"yaw\":" << static_cast<float>(r.estimate.rotation)
      << ",\"truth_x\":" << truth.translation.x() / 1000.f << ",\"truth_y\":" << truth.translation.y() / 1000.f
      << ",\"height\":" << r.frame.position[2]
      << ",\"team_sent\":" << r.teamSent << ",\"team_received\":" << r.teamReceived
      << ",\"behavior\":" << quoted(r.behavior) << ",\"opponents\":[";
  bool firstOpponent = true;
  for(const auto& point : r.opponents)
  {
    if(!firstOpponent) out << ',';
    firstOpponent = false;
    out << "{\"x\":" << point.x() / 1000.f << ",\"y\":" << point.y() / 1000.f << '}';
  }
  out << "],\"graph\":[";
  bool first = true;
  for(const auto& node : r.behaviorGraph)
  {
    if(!first) out << ',';
    first = false;
    out << "{\"option\":" << quoted(node.option) << ",\"state\":" << quoted(node.state)
        << ",\"depth\":" << node.depth << ",\"option_ms\":" << node.optionTime
        << ",\"state_ms\":" << node.stateTime << ",\"arguments\":[";
    bool firstArgument = true;
    for(const auto& argument : node.arguments)
    {
      if(!firstArgument) out << ',';
      firstArgument = false;
      out << quoted(argument);
    }
    out << "]}";
  }
  const auto& field = r.fieldDimensions;
  out << "],\"field\":{\"name\":" << quoted(r.options.field)
      << ",\"half_length\":" << field.xPosOpponentGoalLine / 1000.f
      << ",\"half_width\":" << field.yPosLeftTouchline / 1000.f << ",\"lines\":[";
  first = true;
  for(const auto& line : field.fieldLinesWithGoalFrame.lines)
  {
    if(!first) out << ',';
    first = false;
    out << '[' << line.from.x() / 1000.f << ',' << line.from.y() / 1000.f << ','
        << line.to.x() / 1000.f << ',' << line.to.y() / 1000.f << ']';
  }
  return out.str() + "]}}";
}

MonitorSender::MonitorSender(const std::string& host, int port, bool enabled)
{
  if(!enabled) return;
  addrinfo hints{}, *addresses = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  const int error = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
  if(error) throw std::runtime_error(std::string("Invalid monitor host: ") + gai_strerror(error));
  socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if(socket < 0 || ::connect(socket, addresses->ai_addr, addresses->ai_addrlen) < 0)
  {
    freeaddrinfo(addresses);
    if(socket >= 0) ::close(socket);
    socket = -1;
    throw std::runtime_error("Cannot create monitor UDP socket");
  }
  freeaddrinfo(addresses);
}
MonitorSender::~MonitorSender() { if(socket >= 0) ::close(socket); }
void MonitorSender::update(const Runtime& runtime)
{
  if(socket < 0) return;
  const auto now = std::chrono::steady_clock::now();
  if(lastSend.time_since_epoch().count() && now - lastSend < std::chrono::milliseconds(100)) return;
  lastSend = now;
  const auto packet = monitorSnapshot(runtime);
  if(packet.size() <= 60000) ::send(socket, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
}
}
