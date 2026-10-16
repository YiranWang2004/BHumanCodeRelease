#pragma once
#include "Framework/Module.h"
#include "Representations/Infrastructure/CameraInfo.h"
#include "IMUOrientation.h"
#include "Representations/Infrastructure/JoystickState.h"
#include "Representations/MotionControl/DampingState.h"
#include "Representations/Infrastructure/FrameInfo.h"
#include "Representations/Infrastructure/SensorData/JointSensorData.h"
#include "Representations/Infrastructure/SensorData/InertialSensorData.h"
#include "Representations/Infrastructure/SensorData/SystemSensorData.h"
#include "Representations/Infrastructure/SensorData/KeyStates.h"
#include "Representations/Communication/GameControllerData.h"
#include "Representations/Modeling/RobotPose.h"

MODULE(RCSSSensorProvider,
{,
  PROVIDES(FrameInfo),
  PROVIDES(CameraInfo),
  PROVIDES(JoystickState),
  PROVIDES(DampingState),
  PROVIDES(JointSensorData),
  PROVIDES(InertialSensorData),
  PROVIDES(SystemSensorData),
  PROVIDES(KeyStates),
  PROVIDES(GameControllerData),
  PROVIDES(RobotPose),
});

class RCSSSensorProvider : public RCSSSensorProviderBase
{
  rcss::IMUOrientation orientation;
  double lastIMUTime = -1.;
  double kickoffStarted = -1.;
  int kickoffTeam = -1;
  std::string previousMode;
  void update(FrameInfo&) override;
  void update(CameraInfo&) override;
  void update(JoystickState&) override {}
  void update(DampingState& data) override { data.isDamping = false; }
  void update(JointSensorData&) override;
  void update(InertialSensorData&) override;
  void update(SystemSensorData&) override;
  void update(KeyStates&) override;
  void update(GameControllerData&) override;
  void update(RobotPose&) override;
};
