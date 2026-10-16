#pragma once
#include "Math/Rotation.h"
#include <algorithm>
#include <cmath>

namespace hsl
{
// IMU attitude without a field heading: integrate body-frame angular velocity,
// then align measured gravity with vertical using the shortest 3D correction.
// Keeping the quaternion state avoids Euler yaw's singularity during falls.
class IMUOrientation
{
  Quaternionf orientation = Quaternionf::Identity();
  bool initialized = false;
public:
  Vector3a update(const Vector3f& gyro, const Vector3a& tilt, float dt)
  {
    const Quaternionf measuredTilt = Rotation::Euler::fromAngles(tilt.x(), tilt.y(), 0.f);
    if(!initialized)
    {
      orientation = measuredTilt;
      initialized = true;
    }
    else
    {
      const float angle = gyro.norm() * dt;
      if(angle > 0.f)
        orientation = orientation * Quaternionf(AngleAxisf(angle, gyro.normalized()));
      const Vector3f upInBody = measuredTilt.conjugate() * Vector3f::UnitZ();
      orientation = Quaternionf::FromTwoVectors(orientation * upInBody, Vector3f::UnitZ()) * orientation;
      orientation.normalize();
    }
    const Matrix3f matrix = orientation.toRotationMatrix();
    return Vector3a(std::atan2(matrix(2, 1), matrix(2, 2)),
                    std::asin(std::clamp(-matrix(2, 0), -1.f, 1.f)),
                    std::atan2(matrix(1, 0), matrix(0, 0)));
  }
};
}
