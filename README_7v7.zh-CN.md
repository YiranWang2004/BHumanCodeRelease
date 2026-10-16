# B-Human 2026 接入 rcssservermj：K1/T1 7v7

分支 `feature/rcssservermj-7v7`，基于官方 Code Release 2026 `2e3374ba8`。
服务器为 `/home/robotics/ZJUDancer/rcssservermj` 的同名分支。

本分支现统一维护在 `/home/robotics/ZJUDancer/BHumanCodeRelease`，使用该仓库既有的
个人 fork 远程；原 `test/HSLServer` 分支独立保留。迁移完成后已删除重复目录
`booster_mjlab/BHumanCodeRelease`，后续操作使用上述统一路径。

客户端复用 B-Human 的行为、球估计、行走、踢球、起身及队内通信。新增 TCP 通信、
传感器和视觉输入适配以及 `s7v7` 策略，常规比赛采用 `t231`：1 名守门员、2 名后卫、
3 名中场、1 名前锋。开球、罚球、角球、球门球、边线球及防守定位球都有独立的
7v7 配置，按可用场上人数选择位置子集。原版 5v5、11v11 配置保留。

本场地的全局对手跟踪距离上限为 70 m，覆盖 55×36 m 场地约 65.73 m 的对角线；
局部避障距离仍为 9 m。服务器感知的静态距离实验、精度及限制见
[远距离对手位置实验报告](reports/opponent_range.md)。这不是全场特权对手真值接口，
仍需目标在相机视野中，且在线精度受自身姿态和跟踪过程影响。

## 首次编译

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
python3 -m pip install zstandard
python3 scripts/setup_sim_toolchain.py
scripts/build_sim.sh
cmake --build build_sim --target rcss_protocol_test rcss_imu_test -j4
ctest --test-dir build_sim --output-on-failure
```

工具链安装脚本为 Linux x86_64 下载带 SHA256 校验的 Clang 18，存入被忽略的
`.rcss-toolchain/`。已有 Clang ≥16 时可设置 `RCSS_CXX_COMPILER=/路径/clang++`，跳过
安装脚本。构建脚本在默认工具链不可用时会复用本仓库已有的
`.hsl-toolchain/bin/clang++`；本机使用该编译器验证。

构建脚本初始化官方锁定的 SimRobot、CompiledNN 子模块，并通过官方脚本获取
ONNX Runtime 1.22.0 到 `Util/onnxruntime/lib/`，首次需联网。需要系统 C/C++ 开发库、
CMake、Make；此无界面客户端不依赖 Qt、mold 或 ccache。
输出为 `build_sim/run_player`，可用 `RCSS_BUILD_JOBS=6` 调整并行编译数。

## 启动比赛

当前 RCSS 客户端默认启用大场踢球实验 `--kick-mode fast-max`，K1/T1 均生效。
每次运行踢球网络（包括传球和带球）时统一使用当前版本 policy：strong=1、
overshoot=0、just-hit=1，目标球速取配置上限 3.4 m/s（网络输入经缩放为 0.34）。
忽略请求中的短传距离，保留请求方向；这表示请求强力、快速触球，并不保证实际球速
或最快触球时间，上层选球、走近球与对准过程仍照常执行。
若要恢复原始 command 和新旧踢球网络选择，给 `run_player` 或 `scripts/start_7v7.py`
添加 `--kick-mode native`。
此覆盖仅编译到 RCSS 后端，实机和原 SimRobot 的 command 逻辑不变。

默认 truth 模式还会跳过解除处罚后原版 4 秒站立定位扫视。接近球准备踢球或带球时，
距准备位姿小于 1 m 即持续看球，超过 1.2 m 才允许返回其他头部动作；原版阈值为
0.25/0.31 m。调用者请求 `LookActive` 时也遵守近球优先规则，以减少球观测过期而
退回 Walk policy 的情况。球失踪时仍保留找球回退，无球队员继续观察环境。
measured 模式使用原版返回扫视和近球头部阈值。
本改动不放宽 Kick policy 的球距、观测时效、绕障及运动状态条件，也不保证每次
实际出脚都更快。CSV 的 `behavior` 列记录活动技能和状态，可与 `policy`、
`kick_inferences`、`ball_age` 对照，区分上层等待与实际网络切换。
CSV 末尾的 `play_time` 记录 server 的 `GS(t)` 比赛累计计时（秒），与仿真累计时间
`time` 分开保存；使用 server 原值，不换算为半场倒计时。

终端一启动服务器：

```bash
conda activate robocup_sim
cd /home/robotics/ZJUDancer/rcssservermj
scripts/start_bhuman_7v7.sh --render
```

终端二启动两队：

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
python3 scripts/start_7v7.py
```

默认左队 `BHumanBlue`、右队 `BHumanRed`，各 7 人；球员号 1～7，1 号为守门员。
启动器先确认左队首名球员完成连接，再启动其他球员，避免左右队因进程竞争而互换。
在服务器窗口按 `K` 左队开球，或按 `J` 右队开球。`Ctrl+C` 停止启动器及其所有客户端。
日志和每人 CSV 默认写入新的 `/tmp/bhuman-rcss7v7-日期时间/`。

常用参数：

```bash
# 自动左队开球
python3 scripts/start_7v7.py --kickoff left

# 自定义队名、端口；人数范围 1～7，场地仍为 55×36 米
python3 scripts/start_7v7.py --players 3 --team-a ZJUDancer --team-b BHuman \
  --host 127.0.0.1 --port 60000 --monitor-port 60001

# 测量值加原生 SelfLocator 定位；默认 truth 使用躯干位置和姿态真值
python3 scripts/start_7v7.py --localization measured
```

队内通信保留原生 UDP，队号默认 81、82，使用 UDP 10081、10082；每队队号必须一致，
两队必须不同。默认在同一台计算机运行所有客户端；跨计算机部署时还需配置原生
TeamMessageChannel 的网络通路。TCP 连接到同一服务器并不自动转发这些 UDP 消息。

## 选择 T1 或混合机器人比赛

默认仍为 K1。服务器启动命令相同，由客户端初始化消息选择独立的原生模型：

```bash
# 两队均使用原生 T1BHuman，各 7 人
python3 scripts/start_7v7.py --robot T1

# 左队 T1，右队 K1
python3 scripts/start_7v7.py --robot-a T1 --robot-b K1 --kickoff left

# 单个 T1 客户端
./build_sim/run_player --robot T1 --team BHuman --team-id 81 \
  --num 2 --num-players 7 --field fifa7vs7 --localization truth
```

`--robot T1` 同时选择 `SimulatedT1` 的原版配置、T1 专用网络和 23 关节协议，包含腰部
`q_tj1` / `te1`；不是把 K1 的策略用在 T1 身上。默认依然使用 Server 特权位置，
只有 `--localization measured` 才启用自主定位。T1 同样直接生成到己方半场，
进球后瞬间摆回、站立等待，跳过走入场和走回场。

| 功能 | T1 原版配置 |
|---|---|
| 行走 | `T1_walk.onnx`，365 维输入、14 维输出 |
| 踢球 | `T1_kick_inner.onnx` / `T1_kick_inner_soft.onnx`，62 维输入 |
| 关节控制 | 23 关节，原版 regular/low PD 与力矩上限 |
| 相机 | 640×480，79.2°×63.6° |

模型目录为服务器的 `src/rcsssmj/resources/robots/T1BHuman/`，从原版 `T1.rsi3`
及外观网格转换。总质量 31.73435 kg，场景初始高度 0.670 m，保留源刚体惯量、
关节轴、参考角、限位、转子惯量、碰撞体及传感器位置。T1 的目标关节角按原版
延迟 **14 ms** 执行，速度低通在每个 2 ms 物理步更新；PD 增益和力矩上限立即更新。
原 `T1` 和 `K1` 模型不受这套延迟控制影响。

## 单个客户端与策略模型

```bash
./build_sim/run_player --host 127.0.0.1 --port 60000 \
  --team BHuman --team-id 81 --num 7 --num-players 7 \
  --field fifa7vs7 --localization truth
```

`--num-players` 是该队实际参与人数，所有同队客户端使用同一个值。
连接时选择服务端的独立 `K1BHuman` 模型。客户端按原版 regular/low 映射发送 PD 参数、
关节标定偏移和力矩上限，读取实际关节力矩。无需另行运行 Isaac Gym 或 MachineLearning。

| 功能 | 原版模型 |
|---|---|
| 行走、站立 | `Config/NeuralNets/Walk/K1_walk.onnx` |
| 普通/强踢球 | `Config/NeuralNets/Walk/K1_kick_inner.onnx` |
| 旧柔和踢球 | `Config/NeuralNets/Walk/K1_kick_inner_soft.onnx` |
| 起身 | `Config/NeuralNets/GetUpEngine/` 的 K1 模型 |

踢球模型的观测、推理与切换条件由原版 WalkingEngine 执行。新增推理记录回调仅在
`BUILD_RCSS_SERVER=ON` 构建中启用，不改模型输入输出。CSV 的 `policy`、
`kick_inferences` 表示实际调用了哪些模型；`tactic`、`set_play` 记录原生策略选择。
原版 `MotionInfo::isKicking()` 固定返回 false，不能用它判断踢球模型是否执行。

## 比赛状态、定位与物理边界

默认使用 Server 的特权位置/姿态（`truth`）；只有显式 `--localization measured` 才开启自主定位。

服务器 `BeforeKickOff` 和 `Goal_Left/Goal_Right` 均映射为原生 Initial。
`K1BHuman` 在服务器中直接生成到己方场内，进球后通过合法 beam 瞬间摆回；
等待期间强制站立，跳过开场走入场及进球后走回场。
双方前锋初始距中点 6 米，均在中圈外。服务器开球后，适配层先提供 0.6 秒 Ready、0.6 秒 Set，再进入原生开球逻辑，
让原生行为选择并初始化 7v7 开球配置；这期间保持站立，服务器开球计时继续进行。非开球队
在对方开球状态及原生开球等待期间保持站立。进球后可在服务器允许的暂停期间重新摆位。
比赛半场、加时、比分和定位球由服务端状态转换为 GameControllerData。
服务器的处罚是直接重新摆位，没有伪造定时罚下信息。

`truth` 直接提供 RobotPose；球和障碍物仍来自视觉观测。
`measured` 把球、L/T/X 交点、罚球点及机器人测量交给原生模型与定位器，不使用真值 x/y/yaw
替换定位结果。IMU 适配仅取四元数中的倾斜和陀螺仪，不将真值航向输入测量定位。
这里是服务器理想视觉，不代表实机感知精度。

建议配合服务器 `--bhuman-physics`：2 ms 物理步长、20 ms TCP 周期。客户端默认每网络帧
执行十个 Motion 子帧，但子帧共用最近的传感器样本，区别于原 SimRobot 的 500 Hz 完整
反馈闭环。`--motion-subframes 1` 可用于对照。场地、球和裁判仍来自本服务器。
模型转换与物理配置细节见[服务器中文说明](../../rcssservermj/README_7v7.zh-CN.md)。

## 本机验证

2026-10-16：Release 编译成功，客户端协议/IMU 两项测试通过，服务器 30 项测试通过。
实际启动 7v7 共 14 个客户端，从 BeforeKickOff 经 KickOff_Left 到 PlayOn，
每个球员均收到 6 名队友的消息。约 30 秒仿真中，14 人躯干最低高度均大于 0.41 米，
普通踢球模型在双方均有实际推理记录。非开球队在 KickOff_Left 期间仅有约 11～12 mm
站立漂移。此测试验证连接、通信、状态转换与策略执行，不等同于踢球精度、比赛胜率或
长时间稳定性验证。日志：`/tmp/rcss7v7-integration/`。

另完成 2v2 测量定位、右队开球联调，约 57 秒仿真。两队各收到 1 名队友的消息，
实际运行 `t231`。站立帧的位置误差中位数约 15～225 mm；红队 2 号曾倒地，测试结束
时已恢复至躯干高度 0.462 米。测量定位与运动的长期稳定性仍需进一步比赛测试。
该次记录在 `/tmp/rcss7v7-measured/`。

补齐准备阶段后再次完成 7v7 联调，CSV 确认实际选择 `ownKickOff7v7` /
`opponentKickOff7v7`、`t231`，并执行定位球配置。日志：`/tmp/rcss7v7-final/`。
最后以默认参数启动 2v2，通过测试注入 Goal_Left / Goal_Right 状态验证两种进球方向：
四人均在收到消息后的前五帧内直接回到指定位置（20 ms/帧），等待期间 MotionRequest
始终为 stand，且所有帧的 RobotPose 与 Server 真值一致。站立稳定后的漂移约 3～135 mm。
测试只注入裁判状态，不将其算作真实射门得分。结果：
`/tmp/rcss7v7-placement-validation.log`。

许可证见 `License.md`。This product includes software developed by B-Human (http://www.b-human.de).

## T1 新增模型验证（2026-10-16）

原生 T1 客户端 Release 编译通过，服务器 33 项测试和客户端协议/IMU 两项测试通过。
服务器回归覆盖 T1 的 23 关节参数、惯量、相机和 IMU 位置、14 ms 延迟及即时增益/限幅，
以及混合模型加入、退出重连时的控制队列处理。重新生成 K1BHuman 后，文件内容与之前一致。

两队均为 T1 的 2v2 联调运行约 69 秒仿真，四人均未出现躯干低于 0.25 m 的倒地帧，
每人收到 1 名队友消息，并实际调用了 T1 行走和踢球模型。
另完成左 T1、右 K1 的 7v7 联调，14 人全部连接、每人收到 6 名队友，
双方实际调用各自机器人类型的策略；约 17 秒仿真内未出现倒地帧。
这些是短时功能验证，不作为比赛胜率或长时间稳定性保证。
日志分别位于 `/tmp/bhuman-t1-smoke/` 和 `/tmp/bhuman-t1-mixed7v7/`。
