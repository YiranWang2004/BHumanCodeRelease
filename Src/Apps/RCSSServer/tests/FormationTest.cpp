#include "Modules/BehaviorControl/StrategyBehaviorControl/Behavior.h"
#include "Representations/Modeling/RobotPose.h"
#include "Tools/BehaviorControl/Strategy/BehaviorBase.h"
#include "Framework/Settings.h"
#include "Framework/ThreadFrame.h"
#include "Platform/File.h"
#include "Platform/SystemCall.h"
#include "Streaming/FunctionList.h"
#include "Streaming/TypeRegistry.h"
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>

SystemCall::Mode SystemCall::getMode() { return simulatedRobot; }

namespace
{
void require(bool passed, const char* message) { if(!passed) throw std::runtime_error(message); }

// Supply the real behavior with deterministic, fresh observations and teammate
// records. Exercise its native transitions and assignment, without physics or
// replacing either algorithm with a second implementation in the test.
class FormationFrame : public ThreadFrame
{
  int getPriority() const override { return 0; }
  void init() override {}
  bool main() override { return true; }
  void terminate() override {}
  std::vector<std::string> allocated;
  template<typename T> T& get(const char* name)
  {
    allocated.emplace_back(name);
    return Blackboard::getInstance().alloc<T>(name);
  }
  std::unique_ptr<Behavior> behavior;
public:
  FrameInfo* time;
  FieldBall* ball;
  GameState* game;
  std::vector<Agent> agents;

  explicit FormationFrame(const Settings& settings) : ThreadFrame(settings, "FormationTest", nullptr, nullptr)
  {
    setGlobals();
    File::setSearchPath(settings.searchPath);
    auto& field = get<FieldDimensions>("FieldDimensions");
    field.load();
    auto& parameters = get<BehaviorParameters>("BehaviorParameters");
    InMapFile("behaviorParameters.cfg") >> parameters;
    auto& specs = get<BallSpecification>("BallSpecification");
    InMapFile("ballSpecification.cfg") >> specs;
    auto& extended = get<ExtendedGameState>("ExtendedGameState");
    extended.stateLastFrame = GameState::playing;
    time = &get<FrameInfo>("FrameInfo");
    time->time = 100000;
    ball = &get<FieldBall>("FieldBall");
    game = &get<GameState>("GameState");
    game->state = GameState::playing;
    game->playerState = GameState::active;
    game->playerNumber = 1;
    game->ownTeam.goalkeeperNumber = 1;
    auto& pose = get<RobotPose>("RobotPose");
    pose.translation = Vector2f(field.xPosOwnGoalArea - 150.f, 0.f);
    behavior = std::make_unique<Behavior>(get<BallSearchParticles>("BallSearchParticles"), specs, parameters, extended,
      *ball, field, get<FieldInterceptBall>("FieldInterceptBall"), *time, *game,
      get<IndirectKick>("IndirectKick"), get<TeamBallModel>("TeamBallModel"), get<WalkingEngineOutput>("WalkingEngineOutput"));
    roster(6);
  }
  ~FormationFrame()
  {
    behavior.reset();
    for(const auto& name : allocated) Blackboard::getInstance().free(name.c_str());
  }
  void roster(int fieldPlayers)
  {
    agents.resize(fieldPlayers + 1);
    for(size_t i = 0; i < agents.size(); ++i)
    {
      auto& agent = agents[i];
      agent.number = static_cast<int>(i) + 1;
      agent.isGoalkeeper = i == 0;
      agent.isUpright = true;
      agent.pose = Pose2f(0.f, -24000.f + i * 5000.f, i % 2 ? 3000.f : -3000.f);
      agent.lastKnownPose = agent.pose;
      agent.currentPosition = agent.pose.translation;
    }
  }
  void tick(Strategy::Type strategy, float x, Tactic::Type expected, int frames = 1)
  {
    for(int i = 0; i < frames; ++i)
    {
      time->time += 20;
      ball->positionOnField = ball->endPositionOnField = Vector2f(x, 0.f);
      for(auto& agent : agents)
      {
        agent.timestamp = agent.timeWhenBallLastSeen = agent.timeWhenLastUpright = time->time;
        agent.timeWhenBallDisappeared = time->time;
        agent.ballPosition = agent.pose.inverse() * ball->positionOnField;
        // No fabricated majority: only the tested agent votes for a tactic.
        if(!agent.isGoalkeeper) agent.proposedTactic = Tactic::none;
      }
      behavior->preProcess();
      behavior->update(strategy, agents.front(), agents);
      behavior->postProcess();
      if(agents.front().acceptedTactic != expected)
        throw std::runtime_error("Unexpected tactic at " + std::to_string(time->time) + ": " +
          TypeRegistry::getEnumName(agents.front().acceptedTactic));
      std::set<Tactic::Position::Type> positions;
      for(const auto& agent : agents)
      {
        require(positions.insert(agent.position).second, "Duplicate assigned position");
        require(agent.baseTranslation.allFinite(), "Non-finite base position");
        require(!agent.baseArea.empty(), "Empty assigned Voronoi region");
      }
    }
  }
  void basePoints(Tactic::Type tactic, std::initializer_list<std::pair<Tactic::Position::Type, Vector2f>> original)
  {
    roster(6);
    agents.front().proposedTactic = tactic;
    tick(tactic == Tactic::t033 ? Strategy::attacking7v7 : Strategy::balanced7v7, 0.f, tactic);
    const bool acceptedMirror = agents.front().acceptedMirror;
    for(const auto& [position, oldPoint] : original)
    {
      const auto type = Tactic::Position::mirrorIf(position, acceptedMirror);
      const auto agent = std::find_if(agents.begin(), agents.end(), [type](const Agent& agent) { return agent.position == type; });
      require(agent != agents.end(), "Missing position from the 2025 formation");
      const Vector2f expected(oldPoint.x() * 55.f / 9.f, oldPoint.y() * 6.f * (acceptedMirror ? -1.f : 1.f));
      require((agent->baseTranslation - expected).norm() < .1f, "Normalized base point differs from the 2025 reference");
    }
    const float expectedGoalkeeperX = tactic == Tactic::t033 ? -20180.f : -23650.f;
    require((agents.front().baseTranslation - Vector2f(expectedGoalkeeperX, 0.f)).norm() < .1f, "Incorrect goalkeeper base point");
  }
};

void transitions(const Settings& settings, Strategy::Type strategy, Tactic::Type attacking)
{
  FormationFrame frame(settings);
  frame.tick(strategy, 0.f, Tactic::t222);
  frame.tick(strategy, 999.f, Tactic::t222, 200);
  frame.tick(strategy, 1001.f, Tactic::t222, 149);
  frame.tick(strategy, 1001.f, attacking);
  frame.tick(strategy, 0.f, attacking, 50);
  frame.tick(strategy, -1001.f, attacking, 149);
  frame.tick(strategy, -1001.f, Tactic::t222);
  frame.tick(strategy, 1001.f, Tactic::t222, 149);
  frame.tick(strategy, 1001.f, attacking);
  frame.roster(5);
  frame.tick(strategy, 0.f, attacking, 10);
  frame.roster(4);
  frame.tick(strategy, 0.f, Tactic::t222);

  // Five field players can enter the attacking variant, but not balanced t123.
  frame.roster(5);
  frame.tick(strategy, 0.f, Tactic::t222);
  frame.tick(strategy, 1001.f, Tactic::t222, 149);
  frame.tick(strategy, 1001.f, strategy == Strategy::attacking7v7 ? attacking : Tactic::t222);
  for(int count = 4; count >= 0; --count)
  {
    frame.roster(count);
    frame.tick(strategy, 0.f, Tactic::t222);
  }
}
}

int main()
{
  try
  {
    std::filesystem::current_path(RCSS_ASSETS_DIR);
    FunctionList::execute();
    for(const auto robot : {Settings::k1, Settings::t1})
      for(const char* scenario : {"RCSSTruth", "RCSSMeasured"})
      {
        Settings settings("FormationTest", .002f, robot, 93, Settings::blue, Settings::blue, 1,
                          "RCSS_fifa7vs7", scenario, 0);
        transitions(settings, Strategy::balanced7v7, Tactic::t123);
        transitions(settings, Strategy::attacking7v7, Tactic::t033);
        for(const auto strategy : {Strategy::balanced7v7, Strategy::attacking7v7})
          for(const auto state : {GameState::setupOwnKickOff, GameState::setupOpponentKickOff,
                                  GameState::setupOwnPenaltyKick, GameState::setupOpponentPenaltyKick})
          {
            FormationFrame frame(settings);
            frame.game->state = state;
            frame.tick(strategy, 0.f, Tactic::t231);
            require(frame.agents.front().acceptedSetPlay != SetPlay::none, "Native 7v7 set play not selected");
          }
        FormationFrame frame(settings);
        using P = Tactic::Position;
        frame.basePoints(Tactic::t222, {{P::defenderL, {-3300.f, 350.f}}, {P::defenderR, {-3300.f, -350.f}},
          {P::midfielderL, {-1800.f, 1050.f}}, {P::midfielderR, {-1800.f, -1050.f}},
          {P::forwardL, {1425.f, 1650.f}}, {P::forwardR, {1425.f, -1650.f}}});
        frame.basePoints(Tactic::t123, {{P::defenderL, {-3300.f, 350.f}}, {P::midfielderR, {-1425.f, -350.f}},
          {P::midfielderL, {0.f, 1550.f}}, {P::forwardC, {1425.f, 0.f}},
          {P::forwardL, {2850.f, 2000.f}}, {P::forwardR, {2850.f, -2000.f}}});
        frame.basePoints(Tactic::t033, {{P::defenderL, {-1170.f, 620.f}}, {P::midfielderR, {0.f, -1550.f}},
          {P::midfielderL, {0.f, 1550.f}}, {P::forwardC, {1425.f, -500.f}},
          {P::forwardL, {2850.f, 2000.f}}, {P::forwardR, {2850.f, -2000.f}}});
        frame.tick(Strategy::s7v7, 2000.f, Tactic::t231, 160);
        frame.tick(Strategy::s7v7, -2000.f, Tactic::t231, 160);
      }
    std::cout << "Native 7v7 transitions, 3 s persistence, +/-1 m hysteresis, player-count gates, assignment and current default passed for K1/T1 truth/measured\n";
    return 0;
  }
  catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
