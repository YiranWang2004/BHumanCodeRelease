#include "Model.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

namespace monitor
{
double Robot::age(Clock::time_point now) const { return std::max(0., std::chrono::duration<double>(now - received).count()); }
std::string teamKey(const Json::Value& data)
{
  return data.get("server", "default").asString() + "/" + std::to_string(data["team_id"].asInt()) + "/" + data["team"].asString();
}
std::string primaryBehavior(const Json::Value& data)
{
  const std::set<std::string> actions = {"GoToBallAndKick", "GoToBallAndDribble", "PassToTeammate", "ReceivePass", "ObservePoint", "WalkToPointReady", "DirectKickOff", "GoToBallWaitAndKick", "HandleReturnFromTouchline", "PenaltyTaker", "PenaltyKeeper"};
  for(const auto& node : data["graph"])
  {
    const auto name = node["option"].asString();
    if(name == "HandleStrikerLostBall" && node["state"].asString() != "knownBallPosition")
      return name + ":" + node["state"].asString();
    if(actions.count(name)) return name + ":" + node["state"].asString();
  }
  for(const auto& node : data["graph"])
    if(node["option"].asString() == "HandleGameState" && node["state"].asString() != "playing")
      return "HandleGameState:" + node["state"].asString();
  return data.get("skill", "none").asString();
}
std::string currentPolicy(const Json::Value& data)
{
  return data.get("policy_current", false).asBool() ? data["policy"].asString() : "-- (" + data["phase"].asString() + ")";
}
namespace
{
bool valid(const Json::Value& d)
{
  if(!d.isObject() || d.get("schema", "").asString() != "bhuman-monitor/v1") return false;
  for(const char* key : {"team", "robot", "session", "phase", "motion", "policy", "policy_kind", "mode", "role", "position", "tactic", "set_play", "skill", "localization"})
    if(!d[key].isString() || d[key].asString().size() > 256) return false;
  if(d["team"].asString().empty() || !d["player"].isInt() || d["player"].asInt() < 1 || d["player"].asInt() > 7 || !d["team_id"].isInt()) return false;
  if(!d["sequence"].isUInt64() || !d["policy_current"].isBool() || !d["penalized"].isBool()) return false;
  for(const char* key : {"walk_inferences", "kick_inferences", "get_up_inferences", "team_sent", "team_received"})
    if(!d[key].isUInt64()) return false;
  for(const char* key : {"time", "play_time", "x", "y", "yaw", "ball_x", "ball_y", "ball_age", "height", "policy_age"})
    if(!d[key].isNumeric() || !std::isfinite(d[key].asDouble())) return false;
  if(!d["graph"].isArray() || d["graph"].size() > 256) return false;
  for(const auto& node : d["graph"])
  {
    if(!node.isObject() || !node["option"].isString() || !node["state"].isString() ||
       node["option"].asString().size() > 256 || node["state"].asString().size() > 256 ||
       !node["depth"].isInt() || node["depth"].asInt() < 0 || node["depth"].asInt() > 64 ||
       !node["option_ms"].isInt() || !node["state_ms"].isInt() || !node["arguments"].isArray() || node["arguments"].size() > 64) return false;
    for(const auto& argument : node["arguments"])
      if(!argument.isString() || argument.asString().size() > 1024) return false;
  }
  if(d.isMember("server") && (!d["server"].isString() || d["server"].asString().size() > 512)) return false;
  if(d.isMember("field"))
  {
    const auto& f = d["field"];
    if(!f.isObject() || !f["half_length"].isNumeric() || !f["half_width"].isNumeric() ||
       !std::isfinite(f["half_length"].asDouble()) || !std::isfinite(f["half_width"].asDouble()) ||
       f["half_length"].asDouble() <= 0 || f["half_width"].asDouble() <= 0 ||
       !f["lines"].isArray() || f["lines"].size() > 2048) return false;
    for(const auto& line : f["lines"])
    {
      if(!line.isArray() || line.size() != 4) return false;
      for(const auto& coordinate : line)
        if(!coordinate.isNumeric() || !std::isfinite(coordinate.asDouble())) return false;
    }
  }
  if(!d["opponents"].isArray() || d["opponents"].size() > 128) return false;
  for(const auto& point : d["opponents"])
    if(!point["x"].isNumeric() || !point["y"].isNumeric() || !std::isfinite(point["x"].asDouble()) || !std::isfinite(point["y"].asDouble())) return false;
  return true;
}
}
bool Store::ingest(const std::string& packet, Clock::time_point now)
{
  if(packet.empty() || packet.size() > 60000) { ++rejected; return false; }
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false; builder["allowComments"] = false;
  builder["failIfExtra"] = true; builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value d;
  std::string error;
  try
  {
    if(!reader->parse(packet.data(), packet.data() + packet.size(), &d, &error) || !valid(d)) { ++rejected; return false; }
    const auto key = teamKey(d);
    if(!teams.count(key) && teams.size() >= 16) { ++rejected; return false; }
    auto& robot = teams[key][d["player"].asInt()];
    if(!robot.data.isNull())
    {
      const auto session = d["session"].asString();
      if(std::find(robot.retiredSessions.begin(), robot.retiredSessions.end(), session) != robot.retiredSessions.end()) { ++outOfOrder; return false; }
      if(session == robot.data["session"].asString())
      {
        if(d["sequence"].asUInt64() <= robot.data["sequence"].asUInt64()) { ++outOfOrder; return false; }
      }
      else
      {
        robot.retiredSessions.push_back(robot.data["session"].asString());
        if(robot.retiredSessions.size() > 8) robot.retiredSessions.pop_front();
        robot.events.push_front({d["time"].asDouble(), d["play_time"].asDouble(), "客户端重新连接 / " + d["robot"].asString()});
        robot.signature.clear();
      }
    }
    const auto signature = d["role"].asString() + " / " + primaryBehavior(d) + " / " + currentPolicy(d) + (d["penalized"].asBool() ? " / 受罚" : "");
    if(signature != robot.signature)
    {
      robot.events.push_front({d["time"].asDouble(), d["play_time"].asDouble(), signature});
      while(robot.events.size() > 200) robot.events.pop_back();
      robot.signature = signature;
    }
    robot.data = std::move(d); robot.received = now;
    ++accepted; return true;
  }
  catch(const Json::Exception&) { ++rejected; return false; }
}
Json::Value Store::summary() const
{
  Json::Value result(Json::objectValue);
  result["accepted"] = Json::UInt64(accepted); result["rejected"] = Json::UInt64(rejected); result["out_of_order"] = Json::UInt64(outOfOrder);
  result["robots"] = Json::Value(Json::arrayValue);
  for(const auto& team : teams)
    for(const auto& entry : team.second)
    {
      auto d = entry.second.data;
      d["receive_age"] = entry.second.age();
      d["event_count"] = Json::UInt64(entry.second.events.size());
      result["robots"].append(d);
    }
  return result;
}
}
