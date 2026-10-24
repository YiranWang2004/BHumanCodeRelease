#include "DancerRecovery.h"
#include <onnxruntime_cxx_api.h>
#include <booster/third_party/nlohmann_json/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace rcss
{
struct DancerRecovery::Impl
{
  Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "dancer-getup"};
  Ort::SessionOptions options;
  Ort::Session session{nullptr};
  std::string inputName, outputName;
  std::vector<int64_t> shape;
  std::array<float, 22> neutral{}, trainingKp{}, kd{};
  float scale = 0.f, standingHeight = 0.f, orientationError = 0.f;
  unsigned settle = 0, timeout = 0, successSteps = 0, standingSteps = 0;
  Output output;
  explicit Impl(const std::string& directory)
  {
    const auto root = std::filesystem::path(directory);
    std::ifstream file(root / "getup.json");
    if(!file) throw std::runtime_error("Missing dancer getup.json");
    nlohmann::json cfg; file >> cfg;
    neutral = cfg.at("default_joint_pos").get<decltype(neutral)>();
    trainingKp = cfg.at("kp").get<decltype(trainingKp)>();
    kd = cfg.at("kd").get<decltype(kd)>();
    for(size_t i = 0; i < 22; ++i)
      if(!std::isfinite(neutral[i]) || !std::isfinite(trainingKp[i]) || !std::isfinite(kd[i]) || trainingKp[i] < 0 || kd[i] < 0)
        throw std::runtime_error("Invalid dancer recovery joint configuration");
    scale = cfg.at("action_scale"); settle = cfg.at("settle_steps"); timeout = cfg.at("timeout_steps");
    successSteps = cfg.at("success_steps"); standingHeight = cfg.at("standing_height"); orientationError = cfg.at("orientation_error");
    if(scale != .6f || settle != 20 || timeout != 300 || successSteps != 50 || standingHeight != .53f || orientationError != .05f)
      throw std::runtime_error("Unexpected dancer recovery deployment contract");
    options.SetIntraOpNumThreads(1); options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    session = Ort::Session(environment, (root / cfg.at("model_file").get<std::string>()).c_str(), options);
    if(session.GetInputCount() != 1 || session.GetOutputCount() != 1) throw std::runtime_error("Invalid recovery actor signature");
    const auto inType = session.GetInputTypeInfo(0), outType = session.GetOutputTypeInfo(0);
    const auto in = inType.GetTensorTypeAndShapeInfo(), out = outType.GetTensorTypeAndShapeInfo();
    shape = in.GetShape();
    if(in.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || out.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
       (shape != std::vector<int64_t>{1,72} && shape != std::vector<int64_t>{72}) ||
       (out.GetShape() != std::vector<int64_t>{1,22} && out.GetShape() != std::vector<int64_t>{22}))
      throw std::runtime_error("Recovery actor expects 72 -> 22 floats");
    Ort::AllocatorWithDefaultOptions allocator;
    inputName = session.GetInputNameAllocated(0, allocator).get();
    outputName = session.GetOutputNameAllocated(0, allocator).get();
  }
};
DancerRecovery::DancerRecovery(const std::string& directory) : impl(std::make_unique<Impl>(directory)) { reset(); }
DancerRecovery::~DancerRecovery() = default;
void DancerRecovery::reset()
{
  impl->output = Output(); impl->output.kd = impl->kd; impl->standingSteps = 0;
}
const DancerRecovery::Output& DancerRecovery::step(const DancerPolicy::State& s, double height)
{
  auto& p = *impl; auto& out = p.output;
  std::array<float, 72> observation{};
  if(!std::isfinite(height)) throw std::runtime_error("Non-finite recovery height");
  for(size_t i = 0; i < 3; ++i)
  { observation[i] = static_cast<float>(s.gyro[i]); observation[3+i] = static_cast<float>(s.gravity[i]); }
  for(size_t i = 0; i < 22; ++i)
  {
    observation[6+i] = static_cast<float>(s.q[i]) - p.neutral[i];
    observation[28+i] = static_cast<float>(s.dq[i]);
    observation[50+i] = out.action[i];
  }
  for(float value : observation) if(!std::isfinite(value)) throw std::runtime_error("Non-finite recovery observation");
  out.tau.fill(0.f);
  if(out.status == Status::succeeded || out.status == Status::failed) return out;
  out.observation = observation;
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory, out.observation.data(), 72, p.shape.data(), p.shape.size());
  const char* inputNames[] = {p.inputName.c_str()}, *outputNames[] = {p.outputName.c_str()};
  auto result = p.session.Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
  const auto* raw = result.front().GetTensorData<float>();
  for(size_t i = 0; i < 22; ++i)
  {
    if(!std::isfinite(raw[i])) throw std::runtime_error("Non-finite recovery action");
    out.action[i] = raw[i]; // Do not clip the raw action or feedforward alone.
  }
  if(out.row >= p.settle)
  {
    out.status = Status::executing;
    for(size_t i = 0; i < 22; ++i) out.tau[i] = p.trainingKp[i] * p.scale * out.action[i];
    const double error = s.gravity[0]*s.gravity[0] + s.gravity[1]*s.gravity[1] + (s.gravity[2]+1.)*(s.gravity[2]+1.);
    p.standingSteps = error < p.orientationError && height > p.standingHeight ? p.standingSteps + 1 : 0;
  }
  ++out.row;
  if(p.standingSteps >= p.successSteps) out.status = Status::succeeded;
  else if(out.row >= p.timeout) out.status = Status::failed;
  if(out.status == Status::succeeded || out.status == Status::failed) out.tau.fill(0.f);
  for(float value : out.tau) if(!std::isfinite(value)) throw std::runtime_error("Non-finite recovery torque");
  return out;
}
}
