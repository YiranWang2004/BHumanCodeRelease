#include "DancerPolicy.h"
#include "DancerGeometry.h"
#include "DancerRecovery.h"
#include <booster/third_party/nlohmann_json/json.hpp>
#include <iostream>

int main(int argc, char** argv)
{
  using Json = nlohmann::json;
  try
  {
    if(argc != 3) throw std::runtime_error("Usage: probe ASSETS walk|kick|getup|geometry");
    if(std::string(argv[2]) == "getup")
    {
      rcss::DancerRecovery recovery(argv[1]);
      std::string line;
      while(std::getline(std::cin, line))
      {
        const auto j = Json::parse(line);
        if(j.value("reset", false)) recovery.reset();
        rcss::DancerPolicy::State s;
        s.q = j.at("q").get<decltype(s.q)>(); s.dq = j.at("dq").get<decltype(s.dq)>();
        s.gyro = j.at("gyro").get<decltype(s.gyro)>(); s.gravity = j.at("gravity").get<decltype(s.gravity)>();
        const auto& out = recovery.step(s, j.at("height"));
        std::cout << Json{{"tau",out.tau},{"kd",out.kd},{"action",out.action},{"observation",out.observation},
                          {"row",out.row},{"status",static_cast<int>(out.status)}}.dump() << std::endl;
      }
      return 0;
    }
    if(std::string(argv[2]) == "geometry")
    {
      std::string line;
      while(std::getline(std::cin, line))
      {
        const auto j = Json::parse(line);
        std::cout << Json(rcss::dancerCameraBall(j.at("yaw"), j.at("pitch"), j.at("distance"), j.at("azimuth"), j.at("elevation"), j.value("standard_k1", false))).dump() << std::endl;
      }
      return 0;
    }
    rcss::DancerPolicy policy(argv[1], std::string(argv[2]) == "walk" ? rcss::DancerPolicy::Kind::walk : rcss::DancerPolicy::Kind::kick);
    std::string line;
    while(std::getline(std::cin, line))
    {
      const auto input = Json::parse(line);
      if(input.value("reset", false)) policy.reset();
      rcss::DancerPolicy::State state;
      state.q = input.at("q").get<decltype(state.q)>();
      state.dq = input.at("dq").get<decltype(state.dq)>();
      state.gyro = input.at("gyro").get<decltype(state.gyro)>();
      state.gravity = input.at("gravity").get<decltype(state.gravity)>();
      state.velocity = input.value("velocity", state.velocity);
      state.seesBall = input.value("sees_ball", false);
      state.ball = input.value("ball", state.ball);
      state.direction = input.value("direction", state.direction);
      const auto& result = policy.step(state);
      std::cout << Json{{"observation", policy.observation()}, {"action", policy.action()},
                        {"q", result.q}, {"kp", result.kp}, {"kd", result.kd}}.dump() << std::endl;
    }
  }
  catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
