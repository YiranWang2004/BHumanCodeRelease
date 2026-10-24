#include "DancerPolicy.h"
#include <onnxruntime_cxx_api.h>
#include <booster/third_party/nlohmann_json/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace rcss
{
namespace
{
using Json = nlohmann::json;
template<size_t N> std::array<double, N> numbers(const Json& config, const char* key)
{
  const auto& values = config.at(key);
  if(!values.is_array() || values.size() != N)
    throw std::runtime_error(std::string("Invalid dancer parameter: ") + key);
  std::array<double, N> result;
  for(size_t i = 0; i < N; ++i)
  {
    result[i] = values[i].get<double>();
    if(!std::isfinite(result[i])) throw std::runtime_error("Non-finite dancer parameter");
  }
  return result;
}
}

struct DancerPolicy::Impl
{
  Kind kind;
  Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "dancer"};
  Ort::SessionOptions options;
  Ort::Session session{nullptr};
  std::string inputName, outputName;
  std::vector<int64_t> shape;
  std::vector<float> input;
  std::array<float, 20> lastAction{};
  std::array<double, 22> neutral{};
  std::array<double, 20> scale{};
  std::array<double, 3> limits{}, kickCommand{}, heldBall{};
  double frequency = 0., phase = 0.;
  unsigned updates = 0;
  Command command;

  Impl(const std::string& directory, Kind kind) : kind(kind)
  {
    const auto root = std::filesystem::path(directory);
    std::ifstream stream(root / (kind == Kind::walk ? "amp.json" : "visualkick.json"));
    if(!stream) throw std::runtime_error("Missing dancer policy configuration in " + directory);
    Json cfg; stream >> cfg;
    neutral = numbers<22>(cfg, "default_dof_pos_22");
    command.kp = numbers<22>(cfg, "kp_22");
    command.kd = numbers<22>(cfg, "kd_22");
    if(kind == Kind::walk)
    {
      scale = numbers<20>(cfg, "action_scale_20");
      limits = numbers<3>(cfg, "velocity_limits");
      for(double limit : limits) if(limit <= 0.) throw std::runtime_error("Invalid AMP velocity limit");
      const auto indices = numbers<20>(cfg, "policy_dof_indices_20");
      for(size_t i = 0; i < 20; ++i)
        if(indices[i] != i + 2) throw std::runtime_error("AMP requires serial body joint order");
      if(cfg.at("update_interval") != .02 || !cfg.at("enable_safety_fallback").get<bool>())
        throw std::runtime_error("Unsupported AMP deployment contract");
      input.resize(69);
    }
    else
    {
      // No deployment tuning, filtering or extra observation sampling: retain
      // the checked-in equivariant student's training/export contract exactly.
      if(cfg.at("hist_len") != 50 || cfg.at("frame_dim") != 75 || cfg.at("num_actions") != 20 ||
         cfg.at("policy_dt") != .02 || cfg.at("clip_actions") != 1. ||
         cfg.at("action_scale") != 1. || cfg.at("action_filter") != 1. ||
         cfg.at("dof_velocity_scale") != .1 || cfg.at("ball_pos_scale") != 1. ||
         cfg.at("action_ramp_seconds") != 0. || cfg.at("target_slew_rate") != 0. ||
         cfg.at("torque_limit_scale") != 0.)
        throw std::runtime_error("Unsupported VisualKick deployment contract");
      scale.fill(1.);
      kickCommand = numbers<3>(cfg, "default_command");
      frequency = cfg.at("default_gait_frequency").get<double>();
      if(!std::isfinite(frequency) || frequency <= 0.) throw std::runtime_error("Invalid gait frequency");
      input.resize(50 * 75);
    }
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    session = Ort::Session(environment, (root / cfg.at("model_file").get<std::string>()).c_str(), options);
    if(session.GetInputCount() != 1 || session.GetOutputCount() != 1)
      throw std::runtime_error("Dancer actor must have one input and one output");
    const auto inType = session.GetInputTypeInfo(0), outType = session.GetOutputTypeInfo(0);
    const auto in = inType.GetTensorTypeAndShapeInfo(), out = outType.GetTensorTypeAndShapeInfo();
    shape = in.GetShape();
    const auto outShape = out.GetShape();
    if(in.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
       out.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
       (shape != std::vector<int64_t>{1, static_cast<int64_t>(input.size())} &&
        shape != std::vector<int64_t>{static_cast<int64_t>(input.size())}) ||
       (outShape != std::vector<int64_t>{1, 20} && outShape != std::vector<int64_t>{20}))
      throw std::runtime_error("Unexpected dancer actor tensor signature");
    Ort::AllocatorWithDefaultOptions allocator;
    inputName = session.GetInputNameAllocated(0, allocator).get();
    outputName = session.GetOutputNameAllocated(0, allocator).get();
  }
};

DancerPolicy::DancerPolicy(const std::string& directory, Kind kind) : impl(std::make_unique<Impl>(directory, kind)) { reset(); }
DancerPolicy::~DancerPolicy() = default;
const std::vector<float>& DancerPolicy::observation() const { return impl->input; }
const std::array<float, 20>& DancerPolicy::action() const { return impl->lastAction; }
const std::array<double, 3>& DancerPolicy::velocityLimits() const { return impl->limits; }
void DancerPolicy::reset()
{
  auto& p = *impl;
  std::fill(p.input.begin(), p.input.end(), 0.f);
  p.lastAction.fill(0.f); p.heldBall.fill(0.);
  p.phase = 0.; p.updates = 0; p.command.q = p.neutral;
}

const DancerPolicy::Command& DancerPolicy::step(const State& state)
{
  auto& p = *impl;
  const auto finite = [](const auto& values)
  { return std::all_of(values.begin(), values.end(), [](double x) { return std::isfinite(x); }); };
  if(!finite(state.q) || !finite(state.dq) || !finite(state.gyro) || !finite(state.gravity) ||
     !finite(state.velocity) || !finite(state.direction) || (state.seesBall && !finite(state.ball)))
    throw std::runtime_error("Non-finite dancer observation");
  if(p.kind == Kind::walk)
  {
    if(state.gravity[2] > -.5) throw std::runtime_error("AMP gravity guard: fallen robot");
    for(size_t i = 0; i < 3; ++i)
    {
      p.input[i] = static_cast<float>(state.gyro[i]);
      p.input[3 + i] = static_cast<float>(state.gravity[i]);
      p.input[66 + i] = static_cast<float>(std::clamp(state.velocity[i], -p.limits[i], p.limits[i]));
    }
    for(size_t i = 0; i < 20; ++i)
    {
      p.input[6 + i] = static_cast<float>(state.q[2 + i]) - static_cast<float>(p.neutral[2 + i]);
      p.input[26 + i] = static_cast<float>(state.dq[2 + i]);
      p.input[46 + i] = p.lastAction[i];
    }
  }
  else
  {
    if(state.seesBall && state.ball != std::array<double, 3>{}) p.heldBall = state.ball;
    std::rotate(p.input.begin(), p.input.begin() + 75, p.input.end());
    auto* frame = p.input.data() + p.input.size() - 75;
    std::fill(frame, frame + 75, 0.f);
    if(p.updates) p.phase = std::fmod(p.phase + .02 * p.frequency, 1.);
    for(size_t i = 0; i < 3; ++i)
    {
      frame[i] = static_cast<float>(state.gravity[i]);
      frame[3 + i] = static_cast<float>(state.gyro[i]);
      frame[6 + i] = static_cast<float>(p.kickCommand[i]);
    }
    frame[9] = static_cast<float>(std::cos(2. * std::acos(-1.) * p.phase));
    frame[10] = static_cast<float>(std::sin(2. * std::acos(-1.) * p.phase));
    for(size_t i = 0; i < 20; ++i)
    {
      frame[11 + i] = static_cast<float>(state.q[2 + i] - p.neutral[2 + i]);
      frame[31 + i] = static_cast<float>(state.dq[2 + i] * .1);
      frame[51 + i] = p.lastAction[i];
    }
    frame[71] = static_cast<float>(p.heldBall[0]);
    frame[72] = static_cast<float>(p.heldBall[1]);
    if(p.heldBall != std::array<double, 3>{})
    {
      const double norm = std::hypot(state.direction[0], state.direction[1]);
      if(norm < 1.e-8) throw std::runtime_error("Invalid VisualKick direction");
      frame[73] = static_cast<float>(state.direction[0] / norm);
      frame[74] = static_cast<float>(state.direction[1] / norm);
    }
  }
  if(!finite(p.input)) throw std::runtime_error("Dancer observation exceeds float range");
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory, p.input.data(), p.input.size(), p.shape.data(), p.shape.size());
  const char* inputs[] = {p.inputName.c_str()}; const char* outputs[] = {p.outputName.c_str()};
  auto result = p.session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
  const float* actions = result.front().GetTensorData<float>();
  for(size_t i = 0; i < 20; ++i)
  {
    if(!std::isfinite(actions[i])) throw std::runtime_error("Non-finite dancer action");
    p.lastAction[i] = p.kind == Kind::kick ? std::clamp(actions[i], -1.f, 1.f) : actions[i];
    p.command.q[i + 2] = p.neutral[i + 2] + p.scale[i] * p.lastAction[i];
  }
  ++p.updates;
  return p.command;
}
}
