#pragma once
#include <json/json.h>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace monitor
{
using Clock = std::chrono::steady_clock;
struct Event { double time = 0, playTime = 0; std::string text; };
struct Robot
{
  Json::Value data;
  Clock::time_point received;
  std::deque<Event> events;
  std::deque<std::string> retiredSessions;
  std::string signature;
  double age(Clock::time_point now = Clock::now()) const;
};
std::string teamKey(const Json::Value& data);
std::string primaryBehavior(const Json::Value& data);
std::string currentPolicy(const Json::Value& data);
struct Store
{
  std::map<std::string, std::map<int, Robot>> teams;
  unsigned long accepted = 0, rejected = 0, outOfOrder = 0;
  bool ingest(const std::string& packet, Clock::time_point now = Clock::now());
  Json::Value summary() const;
};
}
