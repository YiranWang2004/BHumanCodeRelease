#include "VisionProvider.h"
#include "Runtime.h"
#include "Math/Rotation.h"
#include <algorithm>
#include <cmath>

MAKE_MODULE(HSLVisionProvider);

namespace
{
Matrix2f covariance(const Vector2f& point)
{
  // Server range noise 1%, angular noise 0.5 deg. Include a 20mm floor for
  // truncation and the conversion from the torso to the ground frame.
  const float range = point.norm();
  const float sigma = 20.f + range * .015f;
  return Matrix2f::Identity() * sigma * sigma;
}
bool isType(const hsl::Object& object, char type) { return !object.type.empty() && std::toupper(static_cast<unsigned char>(object.type[0])) == type; }
}
Vector3f HSLVisionProvider::cameraPoint(const hsl::Object& object) const
{
  const float az = static_cast<float>(object.azimuth * Constants::pi / 180.f);
  const float el = static_cast<float>(object.elevation * Constants::pi / 180.f);
  const float distance = static_cast<float>(object.distance * 1000.);
  return theCameraMatrix * Vector3f(distance * std::cos(el) * std::cos(az), distance * std::cos(el) * std::sin(az), distance * std::sin(el));
}
void HSLVisionProvider::update(BallPercept& data)
{
  data.status = BallPercept::notSeen;
  auto& runtime = hsl::Runtime::get();
  const auto& frame = runtime.frame;
  if(!frame.hasVision) return;
  runtime.visionTime = runtime.timestamp;
  runtime.visualDetected = false;
  if(!theCameraMatrix.isValid) return;
  const hsl::Object* closest = nullptr;
  for(const auto& object : frame.objects)
    if(isType(object, 'B') && object.distance > 0. && (!closest || object.distance < closest->distance)) closest = &object;
  if(!closest) return;
  // Student20 needs the full torso-frame ball, before gravity projection.
  // Use B-Human's camera extrinsics (different from the server's legacy K1).
  runtime.visualDetected = true;
  runtime.visualBall = (theTorsoMatrix.inverse() * cameraPoint(*closest)).head<2>() * .001f;
  runtime.visualBallTime = runtime.timestamp;
  data.status = BallPercept::seen;
  data.positionOnField = cameraPoint(*closest).head<2>();
  data.covarianceOnField = covariance(data.positionOnField);
  data.radiusOnField = theBallSpecification.radius;
  const float az = static_cast<float>(closest->azimuth * Constants::pi / 180.), el = static_cast<float>(closest->elevation * Constants::pi / 180.);
  const float focalX = 272.f / std::tan(52.5_deg), focalY = 224.f / std::tan(47_deg);
  data.positionInImage = Vector2f(272.f - focalX * std::tan(az), 224.f - focalY * std::tan(el) / std::cos(az));
  data.radiusInImage = std::max(1.f, focalX * data.radiusOnField / static_cast<float>(closest->distance * 1000.));
}
void HSLVisionProvider::update(FieldLineIntersections& data)
{
  data.intersections.clear();
  const auto& frame = hsl::Runtime::get().frame;
  if(!frame.hasVision || !theCameraMatrix.isValid) return;
  for(const auto& object : frame.objects)
    if(isType(object, 'L') || isType(object, 'T') || isType(object, 'X'))
    {
      FieldLineIntersections::Intersection intersection;
      intersection.type = isType(object, 'L') ? FieldLineIntersections::Intersection::L : isType(object, 'T') ? FieldLineIntersections::Intersection::T : FieldLineIntersections::Intersection::X;
      intersection.pos = cameraPoint(object).head<2>();
      intersection.cov = covariance(intersection.pos);
      // The server supplies intersection class and position, not line
      // directions. HSL perceptRegistrationProvider disables direction use.
      intersection.dir1 = Vector2f::Zero(); intersection.dir2 = Vector2f::Zero();
      data.intersections.push_back(intersection);
    }
}
void HSLVisionProvider::update(PenaltyMarkPercept& data)
{
  data.wasSeen = false;
  if(!theCameraMatrix.isValid) return;
  for(const auto& object : hsl::Runtime::get().frame.objects)
    if(isType(object, 'P') && (!data.wasSeen || object.distance * 1000. < data.positionOnField.norm()))
    {
      data.wasSeen = true;
      data.positionOnField = cameraPoint(object).head<2>();
      data.covarianceOnField = covariance(data.positionOnField);
    }
}
void HSLVisionProvider::update(FieldBoundary& data) { data.isValid = false; }
void HSLVisionProvider::update(RobotPercept& data)
{
  data.robots.clear();
  if(!theCameraMatrix.isValid) return;
  const auto& r = hsl::Runtime::get();
  for(const auto& player : r.frame.players)
  {
    if(player.parts.empty() || (player.team == (r.left ? "0" : "1") && player.number == r.options.number)) continue;
    const auto& part = player.parts.front();
    RobotPercept::Robot obstacle;
    obstacle.center = cameraPoint(part).head<2>();
    const float distance = obstacle.center.norm();
    if(distance < 1.f) continue;
    const Vector2f width(-obstacle.center.y() / distance * 180.f, obstacle.center.x() / distance * 180.f);
    obstacle.left = obstacle.center + width; obstacle.right = obstacle.center - width;
    obstacle.type = player.team == (r.left ? "0" : "1") ? RobotPercept::ownPlayer : RobotPercept::opponentPlayer;
    obstacle.covariance = covariance(obstacle.center);
    data.robots.push_back(obstacle);
  }
}
