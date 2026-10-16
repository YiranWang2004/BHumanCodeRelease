#include "IMUOrientation.h"
#include <iostream>
#include <stdexcept>

static Vector3a tiltOf(const Quaternionf& q)
{
  const auto m = q.toRotationMatrix();
  return Vector3a(std::atan2(m(2, 1), m(2, 2)), std::asin(std::clamp(-m(2, 0), -1.f, 1.f)), 0.f);
}
int main()
{
  // Complete rotations through vertical with a simultaneous turn. The former
  // Euler yaw integrator loses heading at +/-90 degree pitch on this path.
  for(const Vector3f& gyro : {Vector3f(0.f, 1.f, .4f), Vector3f(1.f, 0.f, .4f)})
  {
    hsl::IMUOrientation imu;
    Quaternionf truth = Quaternionf::Identity();
    imu.update(Vector3f::Zero(), Vector3a::Zero(), 0.f);
    for(int i = 0; i < 6000; ++i)
    {
      truth = (truth * Quaternionf(AngleAxisf(gyro.norm() * .002f, gyro.normalized()))).normalized();
      const Vector3a angles = imu.update(gyro, tiltOf(truth), .002f);
      const Quaternionf estimate = Rotation::Euler::fromAngles(angles);
      if(!angles.allFinite() || truth.angularDistance(estimate) > .002f)
        throw std::runtime_error("Quaternion attitude lost orientation through a fall");
    }
  }
  // Tilt correction cannot read an absolute field heading: the same body
  // gravity results from all rotations of the field about its vertical axis.
  const Quaternionf tilted = Rotation::Euler::fromAngles(.5f, -.8f, 0.f);
  hsl::IMUOrientation a, b;
  const auto x = a.update(Vector3f::Zero(), tiltOf(tilted), 0.f);
  const auto y = b.update(Vector3f::Zero(), tiltOf(Rotation::Euler::fromAngles(0.f, 0.f, 2.f) * tilted), 0.f);
  if((x - y).norm() > 1e-5f) throw std::runtime_error("IMU uses absolute heading");
  std::cout << "Quaternion IMU checks passed\n";
}
