# B-Human 2026 接入 dancer-rcssservermj

分支为 `test/HSLServer`，基于官方 Code Release 2026 `2e3374ba8`。
本地 `master` 也已更新到该版本。之前基于 2025 的适配保存在 Git 备份标签
`backup/HSLServer-2025-before-2026`（包含当时未提交的源码和配置）。

客户端使用今年原生的 Color、Cognition、Motion 模块链路，保留其行为、定位、球估计、
行走、踢球、起身、里程计、速度配置和队内通信。HSL 仅替换硬件输入、图像识别输出及调度入口。
不再使用旧的 `RLWalkingEngine`，也没有补写 WalkKickEngine 踢球关键帧。

## 编译

首次准备本地 Clang 18 工具链，需要 Python 的 `zstandard` 模块、网络、CMake、Make 和
系统 C/C++ 开发库。工具链存放在被忽略的 `.hsl-toolchain/`，构建存放在 `build_sim/`。

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
python3 scripts/setup_sim_toolchain.py
scripts/build_sim.sh
cmake --build build_sim --target hsl_protocol_test hsl_imu_test -j4
ctest --test-dir build_sim --output-on-failure
```

`build_sim.sh` 更新到 2026 锁定的 SimRobot、CompiledNN 子模块，并调用官方
`Make/Common/pullONNXRuntime` 下载该版要求的 ONNX Runtime 1.22.0 到被忽略的
`Util/onnxruntime/lib/`。首次需要网络。可设置 `HSL_CXX_COMPILER` 和 `HSL_BUILD_JOBS`。
HSL 编译不依赖 Qt 界面、Python 绑定、ccache 或 mold。

## 统一启动 3v3、Middle 场地、measured 定位

先启动更新后的服务器：

```bash
cd /home/robotics/ZJUDancer/dancer-rcssservermj
.venv/bin/rcssservermj --host 127.0.0.1 --aport 60000 --mport 60001 \
  --field hsl_m_26 --sync --render
```

再启动左队 ZJUDancer 和右队 B-Human：

```bash
cd /home/robotics/ZJUDancer/dancer-robocupdemo
scripts/start_sim_game.sh --field hsl_m --level foundation \
  --localization measured --team-a ZJUDancer --team-b BHuman \
  --guest-repo /home/robotics/ZJUDancer/BHumanCodeRelease
```

默认 `measured`；`--localization truth` 可切换真值定位。
键盘开球等操作见服务器 README。服务器的 Python editable 安装在修改后重启生效；
需要先更新服务器模型和代码，再启动客户端。

服务器默认罚停统一为 15 秒，通过 `Pen` 消息映射为原生 GameController 处罚状态和
倒计时。原生 `ExtendedGameStateProvider` 在超过 12 秒的罚停解除时发出
`returnFromGameControllerPenalty`，`SelfLocator` 随后重置到场边入场位置假设。
原先服务器的 10 秒罚停低于该门槛，会跳过这次定位重置。罚停期间不会因瞬移立即
强制重定位；重定位发生在解除处罚时，并由视觉进一步确定所在边线。

## B-Human 11v11 自踢

先重新编译本仓库的 `run_player`，再分别在两个终端启动服务器和客户端。
服务器使用新增的 Large 11v11 模式：

```bash
cd /home/yiranwang/rcssservermj
.venv/bin/rcssservermj --field hsl_l_26 --level 11v11 --sync --render
```

客户端复用 demo 的统一入口，`--home-repo` 指向本仓库；未指定 `--guest-repo`
时客队也使用同一份客户端。双方队名必须不同：

```bash
cd /home/yiranwang/dancer-robocupdemo
bash scripts/start_sim_game.sh --field hsl_l --level 11v11 \
  --localization measured --team-a BHumanA --team-b BHumanB \
  --home-repo /home/yiranwang/ZJUDancer/BHumanCodeRelease
```

脚本启动两队各 11 人，1 号为门将。按 Ctrl-C 停止双方客户端。
`hsl_l` 的 `--num-players 8` 至 `11` 自动叠加原生 `Scenarios/11v11` 策略选择，
使用 `s11v11`、`t343`（门将＋3 后卫＋4 中场＋3 前锋）和原生 11v11 定位球配置。
HSL 的线程、measured/truth 定位、运动模块及服务器匹配的场地几何继续使用现有配置。
其他场地仍最多 7 人；不超过 7 人时保留原有 `s5v5` 选择。
Large 入场配置已补齐 8–11 号，分别使用独立的下边线外位置。

直接启动单个球员时，可使用 `build_sim/run_player --field hsl_l --num-players 11 --num 11`，
不需要另传策略参数。启动日志会打印人数及策略，trace CSV 末尾增加
`tactic`、`position`、`role`，用于确认实际接受的阵型、位置与行为角色。
`--role striker` 只是启动参数中的场上球员标记，实际战术角色由原生行为动态分配。

2026-10-17 本机联调：客户端编译及两项 CTest 通过；通过统一脚本启动 measured 11v11，
22 人全部在 Ready 和 monitor 切换的 PlayOn 中接受 `t343`，每人收到 10 名队友，
记录中未同步队友消息数为 0。原有 measured 3v3 回归通过，truth 定位的 11 号球员
完成 200 帧并正常退出。本轮为短时接入、战术和通信验证，未跑完整场比赛。
日志见 `/tmp/bhuman-hsl-roster-s6_pgac4/` 和 `/tmp/bhuman-11v11-truth-9u970_lj/`。

## 2026 原生 policy

无需同时运行 MachineLearning 或 Isaac Gym。B-Human 加载本仓库原生 ONNX：

| 功能 | 模型 |
|---|---|
| 行走、站立 | `Config/NeuralNets/Walk/K1_walk.onnx` |
| `rlKickDynamic`、`rlKickStrong` | `Config/NeuralNets/Walk/K1_kick_inner.onnx` |
| `rlKickDynamicOld` | `Config/NeuralNets/Walk/K1_kick_inner_soft.onnx` |
| 起身 | 原生 `GetUpEngine` 及 `Config/NeuralNets/GetUpEngine/` 中的 K1 模型 |
| 里程计 | 原生 `OdometryDataPreviewProvider` 及其 K1 模型 |

普通/强踢的 `K1_kick_inner.onnx` 与本机 MachineLearning 的
`IsaacGymRL/pre-trained/WalkAndKick/K1_kick_inner.onnx` 逐字节一致，SHA256 为
`7fb9ea33c36e14332861025f22e92f2f4a7ae7681f4532bdd6e1e78975fe3452`。
行走观测为 344 维，踢球观测为 59 维，观测构造和切换条件均由 2026 原版代码执行。

2026 原版的 `MotionInfo::isKicking()` 仍固定为 false，踢球也仍属于 walk 阶段；
不能用这两个字段判断有没有调用 policy。HSL 编译为原生 WalkingEngine 增加了两个
只记录推理事件的回调，不改变模型输入、输出或切换逻辑，普通编译不启用。

设置 `HSL_TRACE_DIR=/tmp/bhuman2026_trace` 后运行统一入口，可以记录每人 CSV。
其中 `policy` 是 WalkingEngine 最近实际执行的行走/踢球模型，`walk_inferences`、`kick_inferences` 是累计推理次数，
`policy_time` 是最近推理的原生时钟。当前阶段可能在两次推理之间变化，请结合 `phase` 查看。
旧字段 `kicking` 保留原生含义，不能当作踢球判据。策略推理次数也不等于成功触球次数。

## 模型、定位和时序

ZJUDancer 使用原服务器 `K1` 模型；B-Human 初始化选择独立 `K1BHuman`，由今年
`Config/Scenes/Includes/K1/K1.rsi3` 转换。新模型总质量 19.666 kg，包含每个关节的
原版转子惯量、完整刚体惯量、碰撞体和限位。相机位置为头部坐标
`(0.0601, 0.0351, 0.0928)m`，分辨率 `544×448`，视场角 `105°×94°`。
客户端使用原生 `motorParameters.cfg`，逐帧发送 regular/low 两组 PD 参数和 `maxTorque`。
服务器只为 `K1BHuman` 按 SimRobot ServoMotor 的顺序执行：目标角限位、速度低通、
PD 求和、动态力矩限幅，最后由 MuJoCo 应用 XML 的固定执行器上限。动态上限不会绕过
固定上限。原位置/速度执行器禁用，滤波后的扭矩仅施加一次。

全场物理步长保持 5 ms，网络帧保持 20 ms。源低通为每 2 ms 的 0.22，按相同时间常数
换算为 `1 - (1 - 0.22)^(5/2) = 0.4626756689`。这保留低通时间常数，不代表复制了
原版的 2 ms 物理闭环。其他模型继续使用原六参数 PD；B-Human 的命令末尾增加可选
第七参数 `maxTorque`（Nm）。缺省时保留当前上限，首次为源 XML 的值。
控制状态在其他机器人加入导致模型重编译时保留，退出重连时重新初始化。

服务器提供关节角、速度、实际关节力矩、IMU 和视觉测量。
2026 `JointSensorData.torque` 直接使用 Nm，不再写入旧版 currents 字段。
measured 模式将球、L/T/X、罚球点和机器人观测交给今年原生模块；坐标转换使用原生
CameraMatrix、RobotCameraMatrix 和 TorsoMatrix。服务器没有提供的线段和交点方向不虚构。
场地配置使用 2026 schema，并按服务器的线中心、标记位置和球尺寸匹配。
服务器的标准 3 号球已全场地统一为 `BallFifa3.rsi3` 的半径 95 mm、质量 0.295 kg；
HSL Middle 和 WHRG 的客户端 `ballSpecification.cfg` 同步采用这组规格。

IMU 适配在三维四元数状态下积分陀螺仪，以重力方向校正倾斜，避免旧欧拉角积分在翻倒时
产生奇异误差；不读取真值航向。原版惯导、定位和里程计算法不作修改。
CSV 中的真值位置、航向仅用于离线诊断，或显式选择 truth 模式。
GC 时间同步采用全队共用的服务器时钟，以 2 Hz 构造包号和接收时间，供原生
GameControllerRBS 同步队友消息；包号不依赖客户端连接先后。

默认每个 20 ms 网络帧执行十个 2 ms Motion 子帧，Color/Cognition 每个网络帧一次。
子帧使用最近一次服务器采样，最后一份命令发送给服务器；这是 50 Hz 网络反馈，
不等同于真实 500 Hz 闭环。`--motion-subframes 1` 保留用于对照。

## 联调记录（2026-10-16）

最终版本已重新编译客户端，通过服务器 52 项测试及客户端 2 项测试。
服务器回归覆盖滤波、两层力矩限幅、旧协议/旧模型行为，以及重编译、退出重连时的状态处理。

统一入口 3v3 运行约 80 秒，使用默认 measured 和十子帧：

| B-Human 球员 | 收到队友数 | kick 推理次数 | 位置误差中位数 | 躯干低于 0.25 m 的帧数 |
|---|---:|---:|---:|---:|
| 1 | 2 | 0 | 5 mm | 0 |
| 2 | 2 | 468 | 10 mm | 0 |
| 3 | 2 | 89 | 21 mm | 0 |

位置误差统计跳过前 200 帧并取躯干高于 0.4 m 的帧。三人均未受罚，未出现队内时钟未同步
而丢弃的消息。这是单次运行，包含随机视觉与比赛过程，不能作为胜率或长时间稳定性结论。
另有 1 m/s 直行及 `(0.3, 0.1, 0.5)` SI 单位的前进/侧移/转向，各运行 30 秒未倒地，
最低躯干高度分别为 0.481 / 0.510 m。

保持原生行为完成 Ready 走位后，在 PlayOn 中分别用 monitor 将机器人置为前、后、侧向
75° 倾斜。三次均由原生 GetUpEngine 恢复站立，最终躯干高度约 0.545 m；最后两秒：

| 倾倒方向 | IMU 相对航向误差中位数 | measured 位置误差中位数 |
|---|---:|---:|
| 前 | 0.40° | 3 mm |
| 后 | 0.27° | 3 mm |
| 侧 | 0.08° | 4 mm |

航向误差在倾倒前对齐参考系。每个方向一次、8000 个服务器帧。该测试通过 monitor 瞬间
摆姿，不是连续外力推倒。另一个固定站立的场边后倒测试中，起身后机器人滑到场外
`y≈5.57 m`，最终位置偏差仍约 0.32 m；没有修改原生定位器去消除这一极端场景的偏差。

独立踢球验证可运行：

```bash
python3 scripts/check_sim_kick.py
```

脚本使用独立 TCP 端口 60220/60221、队号 97（UDP 10097），启动一个采用原生行为和
truth 定位的 B-Human 场上球员，记录真实 MuJoCo 脚球接触与球轨迹，并与实际 kick
推理事件对照。它不修改行为或物理公式，不把裁判重新摆球算成踢球位移；日志输出到
新的 `/tmp/bhuman2026-kick-*` 目录。测试结束只清理自己启动的进程。

本轮实际执行了 282 次 kick 推理，
28 个物理采样记录了 kick 期间的脚球接触。首次接触后 0.5 秒内，球连续移动约
0.454 m。该检查验证 policy 已执行并产生实际触球、球移动，不验证踢球精度或整场胜率。
推理次数和接触采样数均不是“成功踢球次数”。

本机详细日志位于 `/tmp/bhuman_ukf_ab/v2026_servo_isolated_match/`、
`/tmp/bhuman_ukf_ab/v2026_servo_final_fall-*/`、
`/tmp/bhuman2026-kick-7ne12ds_/`。2025 旧版本的测试结论不作为本版本稳定性依据。

许可证见 `License.md`。This product includes software developed by B-Human (http://www.b-human.de).
