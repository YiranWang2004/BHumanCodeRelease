#include "Representations/BehaviorControl/BehaviorStatus.h"
#include "Representations/Modeling/BallModel.h"
#include "Representations/Modeling/RobotPose.h"
#include "Tools/Communication/CompressedTeamCommunicationStreams.h"
#include "Framework/Settings.h"
#include "Framework/ThreadFrame.h"
#include "Platform/File.h"
#include "Platform/SystemCall.h"
#include "Streaming/FunctionList.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

SystemCall::Mode SystemCall::getMode() { return simulatedRobot; }

namespace
{
// Supply normal framework globals to the codec without starting robot threads.
class CodecFrame : public ThreadFrame
{
  int getPriority() const override { return 0; }
  void init() override {}
  bool main() override { return true; }
  void terminate() override {}
public:
  explicit CodecFrame(const Settings& settings) : ThreadFrame(settings, "CodecTest", nullptr, nullptr) { setGlobals(); }
};
void require(bool passed, const char* message) { if(!passed) throw std::runtime_error(message); }
std::string schema(const std::string& location, const std::string& scenario, Settings::RobotType robot)
{
  Settings settings("CodecTest", .002f, robot, 93, Settings::blue, Settings::blue, 4, location, scenario, 0);
  File::setSearchPath(settings.searchPath);
  File file("teamMessage.def", "r");
  require(file.exists(), "Missing team-message schema");
  std::string source(file.getSize(), '\0'); file.read(source.data(), source.size());
  return source;
}
template<typename T>
T roundTrip(const T& source, const CompressedTeamCommunication::Type* type)
{
  require(type != nullptr, "Missing codec type");
  std::vector<std::uint8_t> bytes;
  CompressedTeamCommunicationOut out(bytes, 200000, type, true);
  out << source;
  T result(source);
  CompressedTeamCommunicationIn in(bytes, 200000, type, [](unsigned t) { return t; });
  in >> result;
  return result;
}
RobotPose poseRoundTrip(const RobotPose& source, const CompressedTeamCommunication::Type* type)
{
  std::vector<std::uint8_t> bytes;
  CompressedTeamCommunicationOut out(bytes, 200000, type, true);
  out << RobotPoseCompact(source);
  RobotPose received;
  RobotPoseCompact compact(received);
  CompressedTeamCommunicationIn in(bytes, 200000, type, [](unsigned t) { return t; });
  in >> compact;
  return received;
}
}
int main()
{
  try
  {
    std::filesystem::current_path(RCSS_ASSETS_DIR);
    FunctionList::execute();
    Settings settings("CodecTest", .002f, Settings::k1, 93, Settings::blue, Settings::blue, 4, "RCSS_fifa7vs7", "RCSSTruth", 0);
    CodecFrame frame(settings);
    const auto source = schema("RCSS_fifa7vs7", "RCSSTruth", Settings::k1);
    require(source == schema("RCSS_fifa7vs7", "RCSSMeasured", Settings::k1), "Measured mode uses another wire layout");
    require(source == schema("RCSS_fifa7vs7", "RCSSTruth", Settings::t1), "T1 uses another wire layout");
    require(source == schema("RCSS_fifa7vs7", "RCSSMeasured", Settings::t1), "Measured T1 uses another wire layout");
    CompressedTeamCommunication::TypeRegistry large;
    large.addTypes(source); large.compile();
    RobotPose pose;
    pose.rotation = 0; pose.covariance = Matrix3f::Identity(); pose.timestampLastJump = 199000;
    float maxError = 0;
    for(const Vector2f& position : {Vector2f(-20350.f, -18220.f), Vector2f(20350.f, 18220.f),
                                  Vector2f(-27500.f, 18000.f), Vector2f(27500.f, -18000.f),
                                  Vector2f(-30500.f, -21000.f), Vector2f(30500.f, 21000.f), Vector2f::Zero().eval()})
    {
      pose.translation = position;
      const auto received = poseRoundTrip(pose, large.getTypeByName("RobotPoseCompact"));
      maxError = std::max(maxError, (received.translation - position).norm());
      require((received.translation - position).cwiseAbs().maxCoeff() < 4.1f, "Field-edge pose clipped or precision lost");
      const Vector2f ball(500.f, 0.f);
      const Vector2f sentBall = pose * ball;
      const Vector2f receivedBall = Pose2f(received.rotation, received.translation) * ball;
      require((receivedBall - sentBall).norm() < 15.f, "Nearby teammate ball disagrees after pose compression");
    }
    for(const Vector2f& target : {Vector2f(-74100.f, 74100.f), Vector2f(74100.f, -74100.f), Vector2f(-60000.f, 42000.f)})
    {
      BallState ball; ball.position = target; ball.velocity = Vector2f(2000.f, -1000.f);
      const auto receivedBall = roundTrip(ball, large.getTypeByName("BallState"));
      require((receivedBall.position - target).cwiseAbs().maxCoeff() < 4.1f, "Robot-relative ball clipped across the carpet");
      BehaviorStatus behavior; behavior.walkingTo = target; behavior.shootingTo = -target; behavior.passTarget = 6; behavior.speed = 1500.f;
      const auto receivedBehavior = roundTrip(behavior, large.getTypeByName("BehaviorStatus"));
      require((receivedBehavior.walkingTo - target).cwiseAbs().maxCoeff() < 4.1f, "Walking target clipped");
      require(receivedBehavior.shootingTo.has_value() && (*receivedBehavior.shootingTo + target).cwiseAbs().maxCoeff() < 4.1f, "Shooting target clipped");
      require(receivedBehavior.passTarget == 6, "Pass target corrupted by coordinate layout");
    }
    CompressedTeamCommunication::TypeRegistry original;
    original.addTypes(schema("Default", "Default", Settings::k1)); original.compile();
    pose.translation = Vector2f(-20350.f, -18220.f);
    const auto clipped = poseRoundTrip(pose, original.getTypeByName("RobotPoseCompact"));
    std::cout << "Negative control: source " << pose.translation.transpose() << " decoded " << clipped.translation.transpose() << " error " << (clipped.translation - pose.translation).norm() << " mm\n";
    require((clipped.translation - Vector2f(-12800.f, -12800.f)).norm() < 1.f, "Original schema unexpectedly changed");
    require((clipped.translation - pose.translation).norm() > 9000.f, "Negative control did not reproduce the match error");
    BHumanMessage packet; packet.playerNumber = 4;
    std::vector<char> bytes(packet.sizeOfBHumanMessage()); packet.write(bytes.data());
    BHumanMessage received;
    require(received.read(bytes.data(), bytes.size()), "Current wire version rejected");
    // Header byte combines the version and magic byte. Flipping its low bit
    // changes RCSS version 73 into the old version 72.
    bytes[0] ^= 1;
    require(!received.read(bytes.data(), bytes.size()), "Incompatible old layout was accepted");
    std::cout << "Large-field pose/ball/walk/kick codec, original negative control, schema selection and wire version passed; max pose error " << maxError << " mm\n";
    return 0;
  }
  catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
