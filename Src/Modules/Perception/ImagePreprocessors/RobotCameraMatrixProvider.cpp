/**
 * @file RobotCameraMatrixProvider.cpp
 * This file implements a class to calculate the position of the camera relative to the body.
 * @author Alexander Härtl
 * @author Colin Graf
 */

#include "RobotCameraMatrixProvider.h"

MAKE_MODULE(RobotCameraMatrixProvider);

#ifdef BHUMAN_RCSS_SERVER
extern bool bhumanRCSSStandardK1();
#endif

void RobotCameraMatrixProvider::update(RobotCameraMatrix& robotCameraMatrix)
{
#ifdef BHUMAN_RCSS_SERVER
  if(bhumanRCSSStandardK1())
  {
    RobotDimensions dimensions = theRobotDimensions;
    dimensions.hipToNeckOffset = Vector3f(5.6f, 0.f, 247.9f);
    dimensions.offsetNeckToCamera = Vector3f(60.f, 0.f, 80.f);
    robotCameraMatrix.computeRobotCameraMatrix(dimensions, theJointAngles.angles[Joints::headYaw], theJointAngles.angles[Joints::headPitch], theCameraCalibration);
    return;
  }
#endif
  robotCameraMatrix.computeRobotCameraMatrix(theRobotDimensions, theJointAngles.angles[Joints::headYaw], theJointAngles.angles[Joints::headPitch], theCameraCalibration);
}
