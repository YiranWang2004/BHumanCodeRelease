#include "Model.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void check(bool value, const std::string& reason) { if(!value) throw std::runtime_error(reason); }
Json::Value packet()
{
  Json::Value d;
  d["schema"] = "bhuman-monitor/v1"; d["server"] = "127.0.0.1:60000";
  d["team"] = "测试队"; d["robot"] = "K1"; d["session"] = "first"; d["player"] = 2; d["team_id"] = 81;
  d["phase"] = "walk"; d["motion"] = "walkToBallAndKick"; d["policy"] = "K1_kick_inner.onnx";
  d["policy_current"] = true; d["penalized"] = false; d["sequence"] = Json::UInt64(1);
  d["policy_kind"] = "kick";
  d["mode"] = "PlayOn"; d["role"] = "ballPlayer"; d["position"] = "forward";
  d["tactic"] = "t211"; d["set_play"] = "none"; d["skill"] = "shoot"; d["localization"] = "truth";
  for(const auto* key : {"time", "play_time", "x", "y", "yaw", "ball_x", "ball_y", "ball_age", "height", "policy_age"}) d[key] = 1.;
  for(const auto* key : {"walk_inferences", "kick_inferences", "get_up_inferences", "team_sent", "team_received"}) d[key] = Json::UInt64(1);
  Json::Value n;
  n["option"] = "GoToBallAndKick"; n["state"] = "kick"; n["depth"] = 3;
  n["state_ms"] = 40; n["option_ms"] = 200; n["arguments"] = Json::Value(Json::arrayValue);
  n["arguments"].append("kickLength = 6000 \"test\"");
  d["graph"] = Json::Value(Json::arrayValue); d["graph"].append(n);
  d["opponents"] = Json::Value(Json::arrayValue);
  return d;
}
std::string encode(const Json::Value& d) { Json::StreamWriterBuilder b; b["indentation"] = ""; return Json::writeString(b, d); }
}
int main()
{
  try
  {
    monitor::Store s; auto d = packet(); const auto start = monitor::Clock::now();
    check(s.ingest(encode(d), start), "valid producer packet");
    auto& robot = s.teams.at(monitor::teamKey(d)).at(2);
    check(robot.data["graph"][0]["arguments"][0].asString().find('"') != std::string::npos, "quoted graph arguments");
    check(monitor::primaryBehavior(d) == "GoToBallAndKick:kick", "nested primary behavior");
    check(monitor::currentPolicy(d) == "K1_kick_inner.onnx", "actual kick inference despite walk phase");
    check(!s.ingest(encode(d)), "duplicate packet ignored");
    d["sequence"] = Json::UInt64(0);
    check(!s.ingest(encode(d)) && s.outOfOrder == 2, "out-of-order ignored");
    check(robot.events.size() == 1, "duplicate packets do not generate transitions");
    d["sequence"] = Json::UInt64(2); d["policy_current"] = false; d["phase"] = "getUp";
    check(s.ingest(encode(d), start + std::chrono::seconds(1)), "phase transition");
    check(monitor::currentPolicy(robot.data) == "-- (getUp)", "previous kick must not be shown as current during getup");
    check(std::abs(robot.age(start + std::chrono::seconds(4)) - 3.) < .001, "staleness uses wall time");
    d["session"] = "restart"; d["sequence"] = Json::UInt64(1); d["time"] = .02;
    check(s.ingest(encode(d)), "new client session may reset sequence and simulation time");
    auto old = d; old["session"] = "first"; old["sequence"] = Json::UInt64(1000);
    check(!s.ingest(encode(old)) && robot.data["session"] == "restart", "late old session cannot replace restarted client");
    const auto count = robot.events.size();
    d["sequence"] = Json::UInt64(2); d["graph"][0]["state_ms"] = 140;
    check(s.ingest(encode(d)) && robot.events.size() == count, "duration updates do not spam timeline");
    for(unsigned i = 3; i < 250; ++i)
    {
      d["sequence"] = Json::UInt64(i); d["role"] = i % 2 ? "ballPlayer" : "supporter";
      check(s.ingest(encode(d)), "timeline series");
    }
    d["session"] = "restart-again"; d["sequence"] = Json::UInt64(1);
    check(s.ingest(encode(d)) && robot.events.size() == 200, "bounded timeline including restart entries");
    auto other = d; other["server"] = "127.0.0.1:61000";
    check(s.ingest(encode(other)) && s.teams.size() == 2, "same team at different servers stays separate");
    other["team_id"] = 82; other["team"] = "Other";
    check(s.ingest(encode(other)) && s.teams.size() == 3, "team discovery");
    const auto rejected = s.rejected;
    for(int i = 0; i < 8; ++i)
    {
      auto bad = d;
      switch(i)
      {
        case 0: bad["player"] = 8; break;
        case 1: bad["schema"] = "unknown"; break;
        case 2: bad["x"] = "NaN"; break;
        case 3: bad["graph"][0]["depth"] = -1; break;
        case 4: bad["graph"][0]["arguments"][0] = Json::Value(Json::objectValue); break;
        case 5: bad["policy_current"] = "true"; break;
        case 6: bad["team_sent"] = Json::Value(Json::arrayValue); break;
        case 7: bad["opponents"].append(Json::Value("invalid")); break;
      }
      check(!s.ingest(encode(bad)), "invalid packet rejected: " + std::to_string(i));
    }
    check(!s.ingest("{\"schema\":"), "truncated JSON");
    check(!s.ingest(encode(d) + " trailing"), "trailing garbage");
    check(!s.ingest(std::string(60001, 'x')), "oversized datagram");
    check(!s.ingest("{\"x\":1,\"x\":2}"), "duplicate keys");
    check(s.rejected == rejected + 12, "invalid counter");
    check(s.summary()["robots"].size() == 3, "diagnostic summary");
    std::cout << "Monitor model: ordering, restart, stale policy, timeline, schema and teams passed\n";
    return 0;
  }
  catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
