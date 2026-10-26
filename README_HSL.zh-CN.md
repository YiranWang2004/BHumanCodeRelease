# B-Human 2026 接入 dancer-rcssservermj

适配原理、相对官方代码的逐项差异与教学实验见 [rcssservermj 适配教学文档](RCSSSERVERMJ_ADAPTATION.zh-CN.md)。

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

## B-Human 对踢：一队改用 ZJUDancer AMP + VisualKick

本仓库提供独立入口，无需调用 `dancer-robocupdemo/scripts/start_sim_game.sh`。
两队都运行 B-Human；默认 A 队使用 ZJUDancer 当前 AMP 行走和 VisualKick，B 队使用原生运动 policy。
先按前文启动匹配场地/人数的服务器，再运行：

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
bash scripts/build_sim.sh
bash scripts/start_sim_game.sh \
  --host 127.0.0.1 --port 60000 \
  --field hsl_m --level foundation \
  --localization truth --sim-speed 1 \
  --team-a BHumanZJU --team-b BHumanNative \
  --policy-team a \
  --dancer-repo /home/robotics/ZJUDancer/dancer-robocupdemo
```

- `--policy-team a|b|none`：选择使用 ZJUDancer AMP + VisualKick 的一队，或让双方都用原生运动 policy。
  球队名字只用作标识，不决定 policy；名字必须不同。
- `--field/--level`：Middle/Large foundation 为 3v3、advanced 为 5v5；Small 为 4v4/7v7；
  WHRG 为 5v5；Large 另支持 `--level 11v11`。
  `--num-players N` 可覆盖每队人数，服务器容量也须匹配。1 号为门将。
- `--dancer-python PATH`：指定装有 NumPy、ONNX Runtime 的 Python。
  未指定时依次使用 `K1_PYTHON`、ZJUDancer 的 `.venv/bin/python`、
  `.venv-sim-motion/bin/python`、`python3`。可按 ZJUDancer 文档安装 source_motion 依赖。
- `--log-dir PATH` 指定日志目录；默认每场创建独立 `/tmp/bhuman-selfplay-*`。
  `--dry-run` 只打印双方命令。Ctrl-C 停止本次启动的全部客户端及其推理进程；
  任意客户端提前退出会结束整组，避免缺人后继续跑。

启动前会校验 ZJUDancer 模型清单和加载推理依赖。不是复制一份 ONNX：直接使用
`--dancer-repo` 下 `src/source_motion/config/policies.json`、`models.sha256.json`
和原来的 `source_motion.policies.PolicyBank`，因此切换当前模型后无需重编译 B-Human。
本次接入时配置指向 `models/student_20/model_7900_bhuman_size3.onnx`。
双方机器人均为 `K1BHuman`；VisualKick 的球观测通过 B-Human 相机外参转到躯干坐标，
保留原 Student20 的 20 ms 推理节拍、50 帧历史、隔帧视觉更新、动作缩放及 PD 参数。
网络偶发的 10/30 ms 分包按仿真时钟采样到 50 Hz，不因额外报文重复推进策略历史。

### 己方定位球统一直接进入 policy

替换队的边线球、球门球、角球、直接/间接任意球、开球及点球，均在比赛允许执行、队内协调选出唯一控球者后，
就使用普通 VisualKick 的球距离、球方向、新鲜度和冷却条件触发。
不再等待 `PassToTeammate` 走到等待位置、等队友或调整传球姿态后才发出底层 kick 请求。
满足条件后仍保留相同的 600 ms 启动过渡；尚未满足条件时直接朝球接近/转向，
不执行原生定位球准备站位。服务器暂停发球、处罚及同队会话互斥仍有效；对方定位球及 READY/SET 阶段不会提前触发。
门将执行己方定位球时也走这套公共进入/退出规则，并在首次触球转入 PlayOn 后保持同一会话规则；普通比赛中的门将解围仍保留专用规则。

### 行为及进入/退出规则

替换队的所有 `walkToBallAndKick` 及 `dribble` 请求（原生带球也会调用 kick 模型；
包括传球、射门、门将解围）都禁用原生
kick policy。VisualKick 每帧的目标是定位坐标系中的**对方球门中心**，不使用 B-Human
选择的接球队友或射门落点。战术走位、路径规划、避障、队内通信、定位和角色分配仍由 B-Human 负责。
正常行走、零速站立以及 VisualKick 启动/退出过渡期，统一使用 ZJUDancer AMP policy。
适配器在每帧原生 Motion 规划结束后读取最终 `MotionInfo.speed`，将平移速度从 mm/s
转换为 m/s，角速度保持 rad/s，再交给原版 `AmpVelPolicy`；不另写目标点跟踪器。
原生 `K1_walk.onnx` 的推理被跳过，仅保留规划及步态阶段管理；对照队不受影响。
当前 AMP 模型由共享 `policies.json` 的 `loco.model_file` 决定，当前为
`models/amp_vel/2026-10-04_10-24-37.onnx`。连续行走和站立不重置 AMP 动作历史；
切换到踢球或交回原生运动后重新进入 AMP 时才重置。
跌倒、起身、prepare/playDead 等非行走阶段仍由 B-Human 控制；倾斜超出 AMP 的重力
有效范围时也不发送 AMP 动作。头部继续使用 B-Human 请求，VisualKick 会话内保持既有头部逻辑。
定位及里程计仍为 B-Human 实现，本次没有换成 ZJUDancer 的里程计。
单球员参数 `--kick-policy zjudancer` 为兼容已有命令继续保留，现在同时启用 AMP 和 VisualKick。

会话条件依据当前 `StrikerDecide` / `RLVisionKick` 适配，数值从 ZJUDancer
`brain_node_init.cpp` 的标量默认值及 `config.yaml` 的 strategy 覆盖项读取：

- 场上主攻者在球可用且近于 0.8 m，或距离在 `(0.2, 6.0) m` 且相对球方向绝对值
  小于 `0.6 × 1.3 rad` 时可以启动。启动过渡 600 ms；开始推理前必须有新鲜真实视觉。
- 近球场上球员启动时零速；远球启动先 400 ms 的 0.9 m/s，再 0.3 m/s，
  与当前 `stepDecelerate()` 的实际实现一致。
- 当前真实球观测最大年龄 650 ms；记忆/预测最多 1000 ms，近于 0.45 m 的丢球
  不用于新会话；已启动会话允许 800 ms 短时视觉保持。没有新视觉报文的帧不算丢球。
- 场上球员连续 4 个不同真实观测超出 6 m 才因距离退出；连续丢失控球权、球不可用、
  球出界、比赛禁止动作或跌倒也会退出。远离中的快球及接近阶段持续倾斜会交回追球。
  场上球员不会因 7 s/14 s 或“无进展”超时被强行打断。
- 普通退出保持 140 ms；追球/倾斜交接先释放踢球再由 AMP 稳定 600 ms；
  再进入冷却 600 ms。球丢失退出期间重新看到球可取消退出，保留推理历史。
- 普通门将在己方罚球区前扩 1 m、侧向扩 0.25 m 的区域内，球可用且获得控制权时直接启动，
  不再等待原生 kick/dribble 请求；应用 ZJUDancer 的 5 m 进入距离、
  3.5 m 退出距离、1.5 s 最短距离退出时间及有界进展/轮次规则。

这是 **B-Human 战术 + ZJUDancer 踢球会话** 的对照模式：ZJUDancer 的 `tmImLead`
映射为本机队内唯一控制者，不移植其整套主攻成本算法、门将拦截选点或定位球
传球配合状态机。由于要求始终射向球门，开球传球完成后提前退场的分支也不采用。
队内控制权覆盖接近球、启动和踢球全过程：以距离和转向成本选人，已启动者优先；
接近阶段挑战者必须成本低于当前人的 70% 并持续 500 ms 才换人。失去资格或超过
300 ms 未更新的控制者会释放。未获得控制权但原生策略仍要求追球的场上球员改为
球后侧方接应，避免两人一起冲球。协调账本使用短时文件锁；同队客户端需运行在
同一主机、同一用户下，尚不支持跨机器协调。

启动期间短暂看不到球时停步等待，连续超过 200 ms 才取消，不重新计满 600 ms；
正式进入推理仍要求真实新鲜视觉。仿真是否出界由服务器裁判模式和停止标志决定；
本地估计出界仅记录诊断，不能覆盖服务器的 PlayOn 或合法定位球。这样持续的
视觉偏差也不会让仍在线内的球反复取消会话。
裁判停止、处罚、禁止动作和恢复动作仍立即生效。

启动器默认在日志目录生成 `csv/` 和 `decisions/`。`[DancerKick]` 文本日志记录
会话状态及退出原因；决策 CSV 每帧记录比赛时间、控制者、原生主攻/踢球请求、
进入阻挡条件、球龄、距离、倾斜和具体退出原因。`HSL_TRACE_DIR`、
`HSL_DECISION_DIR` 可分别覆盖输出目录。客户端 CSV 的
`dancer_phase`、`dancer_inferences` 记录新 policy 的阶段和累计推理次数；
新增 `amp_inferences` 计数正常行走、站立及过渡期的 AMP 推理。
原有 `walk_inferences` / `kick_inferences` 仅计数原生模型，替换队两者都应为 0。
需要逐帧诊断时设置 `HSL_DANCER_TRACE_DIR`，会保存以推理进程 PID 命名的 JSONL 输入/输出。

验证会话边界及启动参数：

```bash
../dancer-robocupdemo/.venv-sim-motion/bin/python -m unittest discover -s scripts/tests -v
```

使用不含 ONNX Runtime 的系统 Python 也可运行状态机测试，但实际模型一致性测试会跳过。
本次已通过编译、原有 2 项 CTest、21 项 Python 测试，以及分别替换左队、右队的独立端口 3v3 短场联调：
双方各收到 2 名队友；替换队实际调用 VisualKick 且原生 kick 推理次数为 0，
对照队实际调用原生 kick 且 VisualKick 次数为 0；Ctrl-C 整组退出码为 0。
这是功能接入验证，不代表整场稳定性或比赛效果评估。


### 完整比赛评估

```bash
python3 scripts/evaluate_selfplay.py --output /tmp/bhuman-evaluation --port 61300
```

使用独立的服务器和端口，自动完成两场常规比赛（每场两半场各 20 分钟），
半场间交换场地、开球方并带入比分。服务器每半场到时会结束，因此下半场以
`--phase 1` 重启双方客户端；这是统一的半场重置，不计为额外比赛。
第二场反转首次开球方。启动器逐人等待注册，评估器还核对服务器真实左右队名，
避免加载 policy 较慢导致先后连接顺序颠倒。可加 `--jobs 2` 并行两场。
启动器还将两队的 UDP 队内通信分别放在服务器端口号及其加一，和 TCP 服务互不冲突；
可用 `--team-port-base` 覆盖这两个 UDP 端口。这样不同场次不会共享原生
`10000 + teamNumber` 端口而串话。单机器人入口可用 `--team-port` 显式指定。默认无渲染、同步运行、加速上限 100 倍，实际速度取决于 CPU。
保持默认比赛物理、3 号球、hsl_m_26 foundation 三人制、真值定位和原生运动子帧数。
“原版 B-Human”在这里指同一 HSL/K1BHuman 适配器内的原生步行、踢球和战术，
不是 SimRobot/NAO 平台之间的比较。

`manifest.json` 保存运行参数及二进制/策略摘要；各半场保存服务器球位置、触球、
比分和模式 CSV、六客户端日志、决策日志以及完成状态。`results.json` 仅包含
到达正规半场结束时刻的完整场次；进程异常不会伪记为完赛。
客户端默认按服务器每半场 1200 s 映射剩余时间，单机器人参数 `--half-seconds`
可适配其他时长；本评估不缩短比赛。


### 2026-10-25 完整比赛回归结果

最终版本跑完两场常规比赛：每场 **2 × 20 分钟、3 对 3**，半场交换场地和开球方，
第二场反转首次开球方。使用上述 hsl_m_26 foundation、真值定位、K1BHuman 和默认比赛物理。
下表比分均为 **B-HumanZJU : 原生 B-Human**，下半场列只统计下半场新增进球。

| 场次 | 上半场 | 下半场 | 全场 |
| --- | --- | --- | --- |
| 第 1 场 | 2:13 | 1:7 | **3:20** |
| 第 2 场 | 3:10 | 2:9 | **5:19** |

合计 **0 胜 0 平 2 负，进 8 球、失 39 球**。这是两场实测结果，尚未达到击败原版的水平。
“原版”指同一 HSL 仿真适配器中的原生战术、步行和踢球网络。

完整机器可读结果及运行摘要见
[`scripts/evaluation_results/2026-10-25-selfplay.json`](scripts/evaluation_results/2026-10-25-selfplay.json)。
本机逐帧日志位于 `/tmp/bhuman-evaluation-final-20261025/`，包含服务器轨迹、客户端 CSV、
决策 CSV、模型/二进制摘要及源码快照。四个半场均到达规定的 1200/2400 s 且由服务器进入
GameOver；运行期间代码和模型摘要未变化。排查阶段的中途试跑不计入此表，串话数据已排除。

本次修复与验证：

- 门将入口不再依赖原生底层踢球请求；定位球不再等待原生准备站位。
- 队内唯一控制权覆盖接近、启动和踢球；已启动者优先，非控制者改为接应。
- 启动短暂丢视觉时停车重获；服务器裁判状态决定出界，本地估计出界仅用于诊断。
- 固定注册顺序、隔离不同比赛的队内 UDP 端口，并修正每半场 1200 s 的客户端时钟。
- 32 项 Python 测试、2 项 C++ 测试通过。两场共 942 次启动、675 次进入 active，
  其中门将进入 active 65 次；同队两个 active 会话的时间重叠为 **0 s**，
  因本地“估计出界”退出为 **0 次**。替换队原生步行/踢球推理计数均为 0。

仍然落后的原因与证据：

1. **门将任务与 policy 适用区域尚不一致。** 原生战术会要求门将到门前区域外追球，
   普通门将 VisualKick 又要求球位于门前区域。第 1 场上半场 17:40.28 失球前，
   1 号仍为原生 `playBall` 角色、policy 为 idle，位置约为 `(0.01, -2.65) m`，
   已接近中场（本方球门位于 x=-7 m）。需要统一门将追球边界、解围和回门决策。
2. **高速来球的应急接管不足。** 保留的 600 ms 过渡会消耗防守窗口，已有控球者优先也
   没有门前威胁的紧急抢占。第 2 场上半场 1:36.56 门将开始启动，1:37.16 才进入
   active，1:37.30 即失球，仅留下约 140 ms 的实际踢球时间。应优先设计门前拦截/
   紧急控球权让渡，而不是统一放宽所有入场条件。
3. **视觉和运动稳定性仍中断控制。** 全场有 132 次启动视觉重获超时、51 次球源不可用
   退出、113 次接近阶段不稳定退出。机身高度低于 0.30 m 的累计时间为本队 284.00
   机器人秒、原版 165.68 机器人秒；该指标不是直接的跌倒次数。需要分别验证 AMP、
   VisualKick 和交接流程，当前证据不足以把差距单独归因于某个模型权重。
4. **定位球避让和进攻协同还需适配。** 本队处罚状态进入 35 次、累计 534.24 机器人秒，
   原版为 20 次、300.35 机器人秒；本队另有一次受罚期间重新入场导致计时延长。
   原生传球接应仍按传球意图运行，执行端却固定射向球门中心，两层任务含义不同。
   后续应统一射门/接应任务，并给对方定位球增加符合服务器规则的退让约束。

这些剩余问题属于上层任务与替换运动之间的协调、应急响应及稳定性；
本次结果不能证明仅调整入场距离或取消全部启动过渡就能赢得比赛。

重新生成统计：

```bash
python3 scripts/summarize_selfplay.py /tmp/bhuman-evaluation-final-20261025
```
