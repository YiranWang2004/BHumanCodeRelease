#pragma once
#include "Framework/Module.h"
#include "Protocol.h"
#include "Representations/Infrastructure/CameraInfo.h"
#include "Representations/Infrastructure/FrameInfo.h"
#include "Representations/Configuration/BallSpecification.h"
#include "Representations/Perception/ImagePreprocessing/CameraMatrix.h"
#include "Representations/Perception/BallPercepts/BallPercept.h"
#include "Representations/Perception/FieldPercepts/FieldLineIntersections.h"
#include "Representations/Perception/FieldPercepts/FieldLines.h"
#include "Representations/Perception/FieldPercepts/LinesPercept.h"
#include "Representations/Perception/FieldPercepts/CirclePercept.h"
#include "Representations/Perception/FieldPercepts/PenaltyMarkPercept.h"
#include "Representations/Perception/ImagePreprocessing/FieldBoundary.h"
#include "Representations/Perception/ObstaclePercepts/RobotPercept.h"
#include "Representations/Sensing/TorsoMatrix.h"

MODULE(RCSSVisionProvider,
{,
  REQUIRES(FrameInfo),
  REQUIRES(CameraInfo),
  REQUIRES(TorsoMatrix),
  REQUIRES(BallSpecification),
  REQUIRES(CameraMatrix),
  PROVIDES(BallPercept),
  PROVIDES(FieldLineIntersections),
  PROVIDES(FieldLines),
  PROVIDES(LinesPercept),
  PROVIDES(CirclePercept),
  PROVIDES(PenaltyMarkPercept),
  PROVIDES(FieldBoundary),
  PROVIDES(RobotPercept),
});

class RCSSVisionProvider : public RCSSVisionProviderBase
{
  Vector3f cameraPoint(const rcss::Object&) const;
  void update(BallPercept&) override;
  void update(FieldLineIntersections&) override;
  void update(FieldLines& data) override { data.lines.clear(); }
  void update(LinesPercept& data) override { data.lines.clear(); }
  void update(CirclePercept& data) override { data.wasSeen = false; }
  void update(PenaltyMarkPercept&) override;
  void update(FieldBoundary&) override;
  void update(RobotPercept&) override;
};
