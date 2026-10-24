#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rcss
{
/** The deployment contract uses serial K1 joints, radians and body-frame SI data.
 * Head joints occupy slots 0 and 1 but are never controlled by these actors.
 */
class DancerPolicy
{
public:
  enum class Kind { walk, kick };
  struct State
  {
    std::array<double, 22> q{}, dq{};
    std::array<double, 3> gyro{}, gravity{0., 0., -1.}, velocity{};
    bool seesBall = false;
    std::array<double, 3> ball{};
    std::array<double, 2> direction{1., 0.};
  };
  struct Command
  {
    std::array<double, 22> q{}, kp{}, kd{};
  };
  DancerPolicy(const std::string& directory, Kind kind);
  ~DancerPolicy();
  void reset();
  const Command& step(const State& state);
  const std::vector<float>& observation() const;
  const std::array<float, 20>& action() const;
  const std::array<double, 3>& velocityLimits() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
