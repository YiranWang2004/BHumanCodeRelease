#pragma once
#include "DancerPolicy.h"

namespace rcss
{
/** Exact model_4999 deployment: 72 inputs, 22 raw actions, live server damping. */
class DancerRecovery
{
public:
  enum class Status { settling, executing, succeeded, failed };
  struct Output
  {
    std::array<float, 22> tau{}, kd{}, action{};
    std::array<float, 72> observation{};
    unsigned row = 0;
    Status status = Status::settling;
  };
  explicit DancerRecovery(const std::string& directory);
  ~DancerRecovery();
  void reset();
  const Output& step(const DancerPolicy::State& state, double height);
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
