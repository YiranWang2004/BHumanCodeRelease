#pragma once
#include <array>
#include <cmath>

namespace rcss
{
/** Selected K1 camera site expressed in the full torso frame, in metres.
 * K1BHuman and standard Booster K1 have different optical origins.
 */
inline std::array<double, 3> dancerCameraBall(double yaw, double pitch, double distance, double azimuth, double elevation, bool standardK1 = false)
{
  const double x = (standardK1 ? .06 : .0601) + distance * std::cos(elevation) * std::cos(azimuth);
  const double y = (standardK1 ? 0. : .0351) + distance * std::cos(elevation) * std::sin(azimuth);
  const double z = (standardK1 ? .08 : .0928) + distance * std::sin(elevation);
  const double cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
  return {.0056 + (cp * x + sp * z) * cy - y * sy,
          (cp * x + sp * z) * sy + y * cy, .2479 - sp * x + cp * z};
}
}
