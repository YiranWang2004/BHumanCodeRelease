#include "Runtime.h"
#include "DancerKick.h"
#include "Framework/Robot.h"
#include "Framework/Debug.h"
#include "Platform/SystemCall.h"
#include "Platform/Time.h"
#include "Platform/File.h"
#include "Streaming/FunctionList.h"
#include "Streaming/InStreams.h"
#include "Representations/Configuration/SetupPoses.h"
#include <algorithm>
#include <csignal>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>

SystemCall::Mode SystemCall::getMode() { return simulatedRobot; }

namespace
{
volatile std::sig_atomic_t interrupted = 0;
void signalHandler(int) { interrupted = 1; }
}

class HSLConsole : public ThreadFrame
{
  DebugReceiver<MessageQueue>* connectReceiver(Debug* debug)
  {
    auto* receiver = new DebugReceiver<MessageQueue>(this, debug->getName());
    debug->debugSender->setReceiver(*receiver);
    return receiver;
  }
  int getPriority() const override { return 0; }
  void init() override {}
  bool main() override { return true; }
  void terminate() override {}
  bool handleMessage(MessageQueue::Message message) override
  {
    if(message.id() == idText)
    {
      auto stream = message.text();
      std::string word;
      std::cerr << "[B-Human]";
      while(!stream.eof()) { stream >> word; std::cerr << ' ' << word; }
      std::cerr << '\n';
      return true;
    }
    return ThreadFrame::handleMessage(message);
  }
public:
  HSLConsole(const Settings& settings, const std::string& name, Debug* debug) :
    ThreadFrame(settings, name, connectReceiver(debug), new DebugSender<MessageQueue>(*debug->debugReceiver, debug->getName())) {}
};

int main(int argc, char** argv)
{
  auto& runtime = hsl::Runtime::get();
  hsl::Connection connection;
  std::unique_ptr<Robot> robot;
  bool started = false;
  try
  {
    runtime.options = hsl::arguments(argc, argv);
    auto& options = runtime.options;
    if(options.trace.empty())
      if(const char* directory = std::getenv("HSL_TRACE_DIR"))
      {
        std::filesystem::create_directories(directory);
        options.trace = (std::filesystem::path(directory) / (options.team + "_" + std::to_string(options.number) + ".csv")).string();
      }
    options.assets = std::filesystem::absolute(options.assets).string();
    if(!options.dancerRepo.empty()) options.dancerRepo = std::filesystem::absolute(options.dancerRepo).string();
    if(options.dancerPython.find('/') != std::string::npos) options.dancerPython = std::filesystem::absolute(options.dancerPython).string();
    if(chdir(options.assets.c_str())) throw std::runtime_error("Cannot enter B-Human assets directory");
    hsl::startDancerKick();
    FunctionList::execute();
    Time::initialize(); Time::setSimulatedTime(true);
    Settings settings("SimulatedK1", .02f / options.motionSubframes, Settings::k1,
                      options.teamId, Settings::red, Settings::red, options.number,
                      "HSL_" + options.field, options.localization == "truth" ? "HSLTruth" : "HSLMeasured", 0);
    // The native 11v11 scenario contains only the strategy selection. Overlay
    // it while preserving the HSL field, localization, and motion modules.
    if(options.players > 7)
      settings.searchPath.insert(settings.searchPath.begin(), options.assets + "/Config/Scenarios/11v11/");
    File::setSearchPath(settings.searchPath);
    InMapFile motorFile("boosterProvider.cfg");
    if(!motorFile.exists()) throw std::runtime_error("Missing native K1 boosterProvider.cfg");
    motorFile >> runtime.motors;
    InMapFile parametersFile("motorParameters.cfg");
    if(!parametersFile.exists()) throw std::runtime_error("Missing native K1 motorParameters.cfg");
    parametersFile >> runtime.motorParameters;
    SetupPoses setup;
    InMapFile setupFile("setupPosesProvider.cfg");
    if(!setupFile.exists()) throw std::runtime_error("Missing HSL setup poses");
    setupFile >> setup;
    for(int number = 1; number <= options.players; ++number)
      if(std::none_of(setup.poses.begin(), setup.poses.end(), [number](const auto& pose) { return pose.playerNumber == number; }))
        throw std::runtime_error("Missing HSL setup pose for player " + std::to_string(number));
    if(runtime.motors.jointMapping.size() != 22) throw std::runtime_error("Invalid K1 native motor configuration");
    std::signal(SIGINT, signalHandler); std::signal(SIGTERM, signalHandler);
    connection.connect(options.host, options.port);
    connection.send("(init K1BHuman " + options.team + " " + std::to_string(options.number) + ")");
    std::ofstream trace;
    if(!options.trace.empty())
    {
      trace.open(options.trace);
      if(!trace) throw std::runtime_error("Cannot open trace file");
      trace << "time,mode,penalized,height,truth_x,truth_y,truth_yaw,estimate_x,estimate_y,estimate_yaw,odom_x,odom_y,motion,game_state,teammates,objects,phase,player_state,ball_age,ball_x,ball_y,kicking,gc_state,gc_time,cognition_time,motion_ticks,phase_started,odom_yaw,imu_yaw,native_yaw,gyro_z,cog_odom_x,cog_odom_y,cog_odom_yaw,policy,walk_inferences,kick_inferences,policy_time,pose_quality,team_sent,team_received,team_unsynchronized,tactic,position,role,dancer_phase,dancer_inferences,amp_inferences,play_time,left_score,right_score\n";
    }
    bool beamed = false;
    unsigned frames = 0;
    const auto wallStart = std::chrono::steady_clock::now();
    double simulationStart = -1.;
    while(!interrupted)
    {
      auto frame = hsl::decode(connection.receive());
      if(frame.time < 0.) { connection.send("(syn)"); continue; }
      if(frame.joints.size() != 22 || !frame.hasOrientation)
        throw std::runtime_error("Incomplete K1 sensor frame");
      for(const auto& joint : frame.joints)
        if(!joint.hasTorque) throw std::runtime_error("Server lacks HJ tau: restart the updated dancer-rcssservermj");
      if(simulationStart < 0.) simulationStart = frame.time;
      const auto expectedWall = wallStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((frame.time - simulationStart) / options.speed));
      std::this_thread::sleep_until(expectedWall);
      {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        if(!runtime.sideKnown)
        {
          if(frame.leftTeam == options.team) { runtime.left = true; runtime.sideKnown = true; }
          else if(frame.rightTeam == options.team) { runtime.left = false; runtime.sideKnown = true; }
        }
        runtime.frame = std::move(frame);
        runtime.timestamp = 100000u + static_cast<unsigned>(std::llround(runtime.frame.time * 1000.));
        Time::addSimulatedTime(static_cast<int>(runtime.timestamp - Time::getCurrentSystemTime()));
        ++runtime.sequence; runtime.stage = 1;
        runtime.changed.notify_all();
      }
      if(!started)
      {
        robot = std::make_unique<Robot>(settings, options.team + std::to_string(options.number));
        robot->push_back(new HSLConsole(settings, options.team + std::to_string(options.number), static_cast<Debug*>(robot->front())));
        robot->start(); started = true;
        std::cout << "[HSL] native B-Human K1 model=K1BHuman " << options.team << " #" << options.number << " field=" << options.field << " localization=" << options.localization << " motionSubframes=" << options.motionSubframes
                  << " players=" << options.players << " strategy=" << (options.players > 7 ? "s11v11" : "s5v5") << std::endl;
      }
      {
        std::unique_lock<std::mutex> lock(runtime.mutex);
        if(!runtime.changed.wait_for(lock, std::chrono::seconds(15), [&] { return runtime.completed == runtime.sequence || !runtime.running; }))
          throw std::runtime_error("B-Human module frame timed out; check preceding module-graph errors");
      }
      if(!runtime.dancerFault.empty()) throw std::runtime_error(runtime.dancerFault);
      auto action = runtime.dancerAction.empty() ? runtime.action() : runtime.dancerAction;
      if(runtime.sideKnown && runtime.estimate.translation.allFinite() && std::isfinite(static_cast<float>(runtime.estimate.rotation)))
        action += "(localization " + std::to_string(runtime.estimate.translation.x() * .001f) + " " + std::to_string(runtime.estimate.translation.y() * .001f) + " " + std::to_string(static_cast<float>(runtime.estimate.rotation)) + ")";
      if(runtime.frame.mode == "Initial" && !beamed)
      {
        const auto& pose = setup.getPoseOfRobot(options.number);
        const double x = options.beamExplicit ? options.beamX : pose.position.x() / 1000.;
        const double y = options.beamExplicit ? options.beamY : pose.position.y() / 1000.;
        const double yaw = options.beamExplicit ? options.beamYaw : (pose.turnedTowards - pose.position).angle() * 180. / Constants::pi;
        action += "(beam " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(yaw) + ")";
        beamed = true;
      }
      const auto truth = runtime.truth();
      if(trace)
        trace << runtime.frame.time << ',' << runtime.frame.mode << ',' << runtime.frame.penalized << ',' << runtime.frame.position[2] << ','
              << truth.translation.x() << ',' << truth.translation.y() << ',' << static_cast<float>(truth.rotation) << ','
              << runtime.estimate.translation.x() << ',' << runtime.estimate.translation.y() << ',' << static_cast<float>(runtime.estimate.rotation) << ','
              << runtime.odometry.translation.x() << ',' << runtime.odometry.translation.y() << ',' << static_cast<int>(runtime.motion.motion) << ','
              << runtime.gameState << ',' << runtime.teammates << ',' << runtime.frame.objects.size() << ','
              << static_cast<int>(runtime.motionInfo.executedPhase) << ',' << runtime.playerState << ','
              << (runtime.timestamp - runtime.ball.timeWhenLastSeen) * .001 << ',' << runtime.ball.estimate.position.x() << ',' << runtime.ball.estimate.position.y() << ',' << runtime.motionInfo.isKicking() << ','
              << runtime.gcState << ',' << runtime.gcTime << ',' << runtime.cognitionTime << ',' << runtime.motionTicks << ',' << runtime.motionInfo.lastMotionPhaseStarted << ','
              << static_cast<float>(runtime.odometry.rotation) << ',' << runtime.imuYaw << ',' << runtime.nativeYaw << ',' << runtime.frame.gyro[2] << ','
              << runtime.cognitionOdometry.translation.x() << ',' << runtime.cognitionOdometry.translation.y() << ',' << static_cast<float>(runtime.cognitionOdometry.rotation) << ',' << runtime.policy << ',' << runtime.walkInferences << ',' << runtime.kickInferences << ',' << runtime.policyTime << ',' << static_cast<int>(runtime.estimate.quality) << ',' << runtime.teamSent << ',' << runtime.teamReceived << ',' << runtime.teamUnsynchronized << ','
              << runtime.tactic << ',' << runtime.position << ',' << runtime.behaviorRole << ',' << runtime.dancerPhase << ',' << runtime.dancerInferences << ',' << runtime.ampInferences << ',' << runtime.frame.playTime << ',' << runtime.frame.leftScore << ',' << runtime.frame.rightScore << '\n';
      if(++frames % 100 == 0)
        std::cout << "[HSL] t=" << runtime.frame.time << " pm=" << runtime.frame.mode << " h=" << runtime.frame.position[2]
                  << " pose=" << runtime.estimate.translation.transpose() << " motion=" << TypeRegistry::getEnumName(runtime.motion.motion)
                  << " phase=" << TypeRegistry::getEnumName(runtime.motionInfo.executedPhase)
                  << " lastWalkKickPolicy=" << runtime.policy << " kickInferences=" << runtime.kickInferences
                  << " teammates=" << runtime.teammates << std::endl;
      connection.send(action + "(syn)");
      if(options.maxFrames && frames >= static_cast<unsigned>(options.maxFrames)) break;
    }
    runtime.stop();
    if(started) { robot->announceStop(); robot->stop(); robot.reset(); }
    connection.close(); Time::deinitialize();
    return 0;
  }
  catch(const std::exception& error)
  {
    std::cerr << "[HSL] " << error.what() << std::endl;
    runtime.stop();
    if(started) { robot->announceStop(); robot->stop(); robot.reset(); }
    return interrupted ? 0 : 1;
  }
}
