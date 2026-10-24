# B-Human 2026 接入 rcssservermj：K1/T1 7v7

分支 `feature/rcssservermj-7v7`，基于官方 Code Release 2026 `2e3374ba8`。
服务器为 `/home/robotics/ZJUDancer/rcssservermj` 的 `feature/bhuman-7v7` 分支。

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

## 当前临时模型实验（2026-10-25）

当前 `--robot K1` 临时默认生成服务器的普通 **K1**，其 XML 对齐 server 本地
`test/unfair_kick` 的 `42c8f60`（原生训练力矩上限 ×1.1）。不是 K1BHuman。
`--robot T1` 仍生成 T1BHuman。运动策略选择独立，继续用
`--motion-policy dancer` 启用 AMP / VisualKick / 新起身。

```bash
# 当前默认普通 K1；保存比赛行为日志
for n in {1..7}; do
  ./build_sim/run_player --num "$n" --motion-policy dancer --log &
done
# 单机恢复旧模型，其他参数不变
./build_sim/run_player --num 1 --motion-policy dancer --server-model K1BHuman --log
```

相机位置已随所选模型切换，普通 K1 使用 server 的 ±60° 水平/垂直检测范围，
沿用普通 K1 的 MuJoCo PD 和 XML 合力矩上限；不套用 K1BHuman 的速度滤波和较低力矩上限。
启动信息、CSV 和监视数据中的 `server_model` 标识实际模型。
双队脚本 `scripts/start_7v7.py` 同样支持 `--server-model`。

服务器 fifa7vs7 的完整球物理已对齐 `test/unfair_kick`：半径 0.11 m、质量 0.41 kg、
各轴惯量 0.0019844 kg·m²、摩擦 (0.4, 0.01, 0.01)、solref (−5000, −20)。
客户端 `RCSS_fifa7vs7/ballSpecification.cfg` 质量也同步为 0.41 kg。
重力对齐 −9.81 m/s²、空气阻力为零；地面和 2 ms / Euler 求解配置保留。
这不是切换整个 server 到 unfair_kick 分支；重启 server 和客户端生效，无须重新编译。
历史 K1BHuman 测试结果不代表普通 K1 的整场表现。

## 首次编译

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
python3 -m pip install zstandard
python3 scripts/setup_sim_toolchain.py
scripts/build_sim.sh
cmake --build build_sim --target rcss_protocol_test rcss_imu_test rcss_team_message_test rcss_formation_test rcss_dancer_gate_test -j4
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

直接运行 `build_sim/run_player` 默认不保存日志。加 `--log` 时，将每帧 CSV 保存到
本仓库根目录的 `logs/`，文件名包含队名、机器人类型、球员号、启动时间和进程号，
重复启动不会覆盖之前的日志。`logs/` 已加入 `.gitignore`；不再读取 `RCSS_TRACE_DIR`。

```bash
# 单队 K1，默认不保存
for n in {1..7}; do ./build_sim/run_player --num "$n" & done
# 单队 K1，保存行为及比赛时间 CSV
for n in {1..7}; do ./build_sim/run_player --num "$n" --log & done
```

T1 在上述命令中添加 `--robot T1`。诊断用 `--trace FILE` 仍可显式指定 CSV 路径，
与 `--log` 互斥。下文的双队测试启动器仍显式通过 `--trace` 保存其测试日志。

### 可选 Dancer 行走、踢球与起身（K1）

默认 `--motion-policy native` 保持原有策略。指定 `--motion-policy dancer` 时，
走路及站立改用 AMP velocity，踢球改用 `5070Ti_equivariant_student.onnx`，
起身改用 `recovery_mjlab/model_4999.onnx`。三套模型和部署配置来自 `dancer-robocupdemo` 的 `test/7vs7` 分支
`aba39596950847771d9ab558a00afab995ff8335`，已随仓库放入 `Config/NeuralNets/Dancer/`，
运行不依赖旁边的 dancer 仓库、Python 运控进程或 ROS。

```bash
scripts/build_sim.sh
for n in {1..7}; do
  ./build_sim/run_player --num "$n" --motion-policy dancer &
done
```

可叠加 `--log`、`--monitor` 和 `--strategy balanced|attacking`。去掉
`--motion-policy dancer` 即恢复原策略。`scripts/start_7v7.py` 的同名参数作用于双方；
新策略仅支持 K1，和 `--robot T1` 同时使用会在连接服务器前报错。

本节原始接入使用 **K1BHuman**；当前临时默认已改为普通 **K1**，以文首实验说明为准。
每 20 ms 推理一次，身体 20 个关节使用模型的默认姿态、动作缩放和 Kp/Kd。
行走、站立和踢球期间保留 B-Human 头控；起身期间由新 policy 接管全部 22 个关节，
交还 AMP 后恢复 B-Human 头控。摔倒检测和进入起身的时机仍由 B-Human 决定。
AMP 接收 B-Human 行走规划产生的速度（m/s、rad/s），部署限幅为
`vx ±2.5、vy ±1.75、yaw ±3`，来自 `Config/NeuralNets/Dancer/amp.json`。
普通 **K1 + dancer** 模式同时以该配置的 vx 上限设置规划层快速前进、最大前进
和前进目标速度映射，当前为 **2.5 m/s**。正常速度档仍为 1.25 m/s，后退、横移、
转向、加减速及组合运动限制沿用原配置；`native` 和 `K1BHuman` 的规划速度不变。
这项调整不改变 VisualKick 的固定 command，也不改变下述交接时序。
单机实测实际直行约 2.14 m/s（原约 1.75 m/s）；2.5 档通过本轮两向交接测试，
但交接姿态波动有所增大，尚未证明整场比赛稳定性等同原配置。
详细条件、数据及失败个例见 [AMP 速度与踢球衔接实验](reports/dancer_amp_speed_results.json)。

VisualKick 的方向使用 B-Human 当前射门、传球或解围方向，经里程计和机身姿态转换。
固定 command 为 `(1,0,1)`，步频 1.8 Hz。它没有原 Kick 的力度、球速或
`allow_deviation` 输入，因此 `--kick-mode` 不调整这个模型，也不保证按原传球距离出球。
球观测来自 server 相机检测，使用**当前 server 模型的相机几何**转换为完整身体坐标；
丢帧时保持最近测量，不二次采样、不用全局球真值替代，也不使用外推后的 BallModel
作为网络输入。首次进入用 49 个零帧和当前帧初始化 50×75 历史，动作和相位清零。

仅新模式使用以下接管条件，truth 和 measured 均适用。过渡规则对齐
`dancer-robocupdemo` 的 `test/7vs7`（`aba3959695`）：

- 当前视觉看到球、身体坐标水平球距 ≤6 m、球偏角绝对值 ≤0.65 rad（约 37°），
  且 B-Human 请求处理球、有球队主罚权并获裁判许可，先以 AMP 零速保持 300 ms。
  期间持续检查视觉、角度、距离和主罚权，失去条件则取消并冷却。
- 进入后保持 Kick 会话；普通 walk/stand 请求、避障路径、截球和让球标记均不直接
  打断它，不再使用 `native_motion_guard`。B-Human 仍选择射门/传球方向；瞬时切换为
  非处理球请求时保持先前的场地坐标踢球方向。头控仍由 B-Human 负责。
- 失去球队处理球许可、球姿态异常持续 600 ms（球距 >0.10 m 且球 x<−0.10 m 或
  偏角 >1.75 rad）、球距 >8.5 m 持续 600 ms 且本轮已执行 >1500 ms、
  无可用球源持续 1000 ms、5 秒无进展或会话满 14 秒，启动退出准备。
  单轮超过 7 秒时，有可用球源可延长一轮，最多一次。
- “进展”指球距缩短 ≥0.12 m、球沿基准踢球方向前进 ≥0.15 m、机器人平移 ≥0.20 m
  或转向 ≥0.25 rad，任一满足就重置进展基线和计时。
- 退出准备持续 140 ms，仍由 Kick 输出动作，发送的 Walk 速度为零；这与 dancer
  的调用顺序一致，**不表示 Kick 固定 command 已减速**。之后切 AMP 零速，下一帧
  恢复规划速度；冷却从实际恢复 Walk 开始计 1200 ms，再次进入仍需 300 ms 准备。
- 裁判停止、对方定位球、罚下立即禁止 Kick；摔倒和起身继续由原优先级接管。
  模型历史只在实际策略切换或中断后重置，不因瞬时运动请求变化而清空。

可用球源采用 dancer 的视觉时间语义：See 每 40 ms 到达，检测标记在消息之间保持；
新鲜观测最多 650 ms，有限球估计可保留至最后观测后 1000 ms，但未检出且球距
≤0.45 m 时不采用记忆。再连续 1000 ms 无可用源才退出，因此不是简单“最后看到球后
恰好 1 秒退出”。B-Human BallModel 仅用于过渡判断/进展统计，网络球输入仍只用相机测量。
球队仲裁沿用 B-Human 的处理球角色或策略层处理球技能，不移植 dancer 自己的队内 cost 仲裁。

起身沿用 dancer 当前部署语义：先 20 帧（0.4 秒）阻尼落稳，再输出力矩；
机身重力投影 `gravity_z < -0.85`（倾斜约小于 32°）且根高度 ≥0.30 m 时，
下一控制帧交给 AMP 完成站直。单次 300 帧（6 秒）仍未完成，则释放力矩一帧后
清空状态重试。起身输出 `Kp=0`、配置 Kd 和 `tau=训练Kp×0.6×action`，
由 server 在每个物理子步计算阻尼并限制合力矩，未把力矩动作转换为位置目标。

CSV 和监视数据记录实际模型名、`motion_policy`、`dancer_gate`、
`dancer_exit_reason`（最近一次退出原因）及 `dancer_ball_owner`。
`dancer_kick_direction_world` 记录会话实际采用的场地坐标踢球方向（rad）。
`dancer_native_flags` 记录最近一次走路/踢球推理时的约束，仅供诊断：按位 1=截球、2=让球、4=避障、8=停止步、16=站立阶段；
这些标记不再决定 Dancer Kick 的退出。CSV 的
`dancer_walk_vx/vy/yaw` 是送往 AMP 接口的规划速度；Kick 执行期间网络实际使用上述固定
command。`kick_flags_available=0` 表示原四个 Kick 指令字段不适用于新模型，不能把它们
显示的零解释为低力度。起身另记录 `dancer_recovery_state/row/attempts`；
CSV 的 `getup_inferences` 为累计起身推理次数，`gravity_z` 为机身重力竖直分量。
非起身帧的 recovery state 为 `inactive`，row/attempts 保留最近一次起身记录。

已完成数值对照、相机几何检查、接管边界测试、独立走路/踢球和起身实验。
目前新模式在 K1BHuman 上的侧走及小角速度响应较弱，短场景接入通过不代表整场比赛
性能优于原策略。实验数据与复现命令见 [Dancer 策略接入报告](reports/dancer_motion.md) 和
[起身策略接入报告](reports/dancer_recovery.md)。本次完整过渡规则及复测见
[Walk / VisualKick 过渡报告](reports/dancer_transitions.md)。

### 可选的动态 7v7 阵型

默认 `--strategy current` 保持现有 `t231`（2-3-1）。新增两种可选策略，
只改变常规比赛的阵型、基准站位及减员时位置优先级，使用今年的角色、动态跑位、
持球者选择和队内投票算法；开球、界外球、角球、任意球、球门球及点球继续使用
本分支现有的 7v7 定位球配置，定位球结束后恢复所选策略。

| 参数 | 常规比赛阵型 | 转入进攻阵型 | 退回 t222 |
| --- | --- | --- | --- |
| `--strategy current` | 固定 t231 | 不切换 | 不切换 |
| `--strategy balanced` | t222 ↔ t123 | 至少 6 名场上队员，球在 x ≥ +1 m 一侧持续 3 秒 | 不超过 4 名场上队员，或球在 x ≤ −1 m 一侧持续 3 秒 |
| `--strategy attacking` | t222 ↔ t033 | 至少 5 名场上队员，球在 x ≥ +1 m 一侧持续 3 秒 | 不超过 4 名场上队员，或球在 x ≤ −1 m 一侧持续 3 秒 |

人数不包含门将，依据原生行为模块维护的有效队员列表；x 为本队坐标，朝对方球门为正。
条件成立后还需经过现有队内投票。只有 5 名场上队员时，balanced 不会从 t222 转入
t123，但已经处于 t123 时也不会仅因人数降为 5 而退出。球在中线附近时保持已有阵型。

```bash
# 常规动态 2-2-2 ↔ 1-2-3；整队统一选择
for n in {1..7}; do ./build_sim/run_player --num "$n" --strategy balanced & done
# 更激进的 t222 ↔ t033
for n in {1..7}; do ./build_sim/run_player --num "$n" --strategy attacking & done
```

K1/T1、truth/measured 均支持；T1 添加 `--robot T1`。双队测试启动器
`scripts/start_7v7.py` 也支持同名参数，应用于它启动的两队。所有同队客户端须选同一种
策略，混用时原生投票无法保证遵循某一套切换规则。监视数据新增 `strategy` 字段，
现有 `tactic` 字段显示本帧实际采用的阵型，定位球期间可能仍显示 t231。

新增三个阵型的场上基准点取自 Code Release 2025 默认 9×6 m 场地，按
`x_new = x_old × 当前场长 / 9 m`、`y_new = y_old × 当前场宽 / 6 m` 映射。
配置直接引用当前球门线和边线尺寸；原来的站位朝向由今年的角色算法自行计算。
当前 55×36 m 场地对应以下基准点（米；实际分配可左右镜像，实际目标还会动态调整）：

| 阵型 | 后卫 | 中场 | 前锋 |
| --- | --- | --- | --- |
| t222 | (−20.17, ±2.10) | (−11.00, ±6.30) | (+8.71, ±9.90) |
| t123 | (−20.17, +2.10) | (−8.71, −2.10)、(0, +9.30) | (+8.71, 0)、(+17.42, ±12.00) |
| t033 | (−7.15, +3.72) | (0, ±9.30) | (+8.71, −3.00)、(+17.42, ±12.00) |

保留 2025 的 `t033` 名称以便对照；它实际仍有 1 名后卫、2 名中场、3 名前锋。
t222/t123 门将基准点沿用当前配置；t033 前移至当前己方罚球点 (−20.18, 0)。
门将继续执行今年的 `Goalkeeper`：常规比赛主要根据球和球门计算目标，通常不会直接
走向该基准点，因此此次调整不能等同于恢复 2025 的 `attackingGoalkeeper` 行为。
配置、原生状态机和独立 server 启动验证见[阵型验证记录](reports/formation_options.md)。

### 实时桌面监视器

首次编译需要系统 SFML 2.5、JsonCpp 开发库和中文字体（Ubuntu：
`sudo apt install build-essential cmake libsfml-dev libjsoncpp-dev fonts-noto-cjk`）。
客户端本身不新增这些界面依赖；桌面程序单独编译：

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
scripts/build_monitor.sh
scripts/start_monitor.sh
```

服务器照常启动，另一个终端启动需要监视的队员：

```bash
for n in {1..7}; do ./build_sim/run_player --num "$n" --monitor & done
```

T1 添加 `--robot T1`。默认发送到本机 UDP `10190`；不加 `--monitor` 不发送。
`--monitor` 与 `--log` 独立，仅打开监视器不会保存日志。

远程查看时，在监视电脑运行 `scripts/start_monitor.sh --port 10190`，
在比赛电脑将上面的客户端命令添加 `--monitor-host <监视电脑IP> --monitor-port 10190`。
监视器默认监听 `0.0.0.0`，也可用 `--listen` 指定本机地址。UDP `10190` 是新增的
遥测端口，与服务器裁判接口的 TCP `60001` 不同。

界面包含 7 人总览、实际配置场地、选中机器人的完整活动行为树、角色/行为/网络切换时间线。
点击机器人卡片或球场机器人，或按 `1`–`7` 选人；`Tab` 切换已发现球队；在行为树或
时间线区域滚轮查看，长文本悬停展开。详细字段和状态语义见
[监视器说明](tools/monitor/README.zh-CN.md)。第一版仅实时查看，不提供离线回放。

### 大场踢球与 truth 行为

RCSS `truth` 模式下，K1/T1 的所有踢球网络调用统一使用当前版本 policy，
包括射门、传球、解围、带球触球、开球及各类定位球：
`strong=1`、`inaccurate=0`、`allow_deviation=0`，目标球速取配置上限 3.4 m/s
（网络输入经缩放为 0.34）。保留请求方向，忽略请求中的短传距离。
这对应“提前接管、保留 policy 学到的姿态约束”：上层跳过面向球门的固定球后 0.5 m
准备位，以及点球大战独立的 WalkToPoint 准备步，由原有踢球流程和 Kick policy
根据最终出球方向完成接近、绕球和触球调整。原有定时扫视等待继续跳过。

这里的姿态约束是训练奖励所对应的学习行为，并非新增固定站立时长或角度硬门槛。
最大目标球速也不保证每次实际球速或触球时间。
原始 48 次速度/精度对照见 [Kick policy 接管实验](reports/kick_alignment.md)。

`--kick-mode fast-max|native` 现在只控制 `measured` 模式：默认 `fast-max` 仍使用
原来的 `(strong,inaccurate,allow_deviation)=(1,0,1)`；`native` 恢复原始 command 和
新旧踢球网络选择。truth 始终使用上述 `(1,0,0)` 模式，即使指定 `--kick-mode native`。
该覆盖仅编译到 RCSS 后端；实机、原 SimRobot 和 measured 的行为流程保持原样。
本次接管入口、实际 command、点球大战和 measured 对照的验证记录见
[truth 模式回归结果](reports/truth_kick_aligned.json)。

默认 truth 模式还会跳过解除处罚后原版 4 秒站立定位扫视及策略层 3.5 秒的恢复等待。接近球准备踢球或带球时，
距准备位姿小于 1 m 即持续看球，超过 1.2 m 才允许返回其他头部动作；原版阈值为
0.25/0.31 m。调用者请求 `LookActive` 时也遵守近球优先规则，以减少球观测过期而
退回 Walk policy 的情况。球失踪时仍保留找球回退，无球队员继续观察环境。
measured 模式使用原版返回扫视和近球头部阈值。
本改动不放宽 Kick policy 的球距、观测时效、绕障及运动状态条件，也不保证每次
实际出脚都更快。CSV 的 `behavior` 列记录活动技能和状态，可与 `policy`、
`kick_inferences`、`ball_age` 对照，区分上层等待与实际网络切换。
CSV 的 `play_time` 记录 server 的 `GS(t)` 比赛累计计时（秒），与仿真累计时间
`time` 分开保存；使用 server 原值，不换算为半场倒计时。
CSV 还记录最近一次实际 Kick 推理的 `kick_strong`、`kick_inaccurate`、
`kick_allow_deviation` 和 `kick_target_speed`（m/s），可在 `kick_inferences` 增加的行
检查输入。这些值会缓存到下一次 Kick 推理，不能仅凭它们判断当前仍在踢球。

RCSS `truth` 模式还会跳过主罚前同类的定时站立扫视：边线球、角球、直接/间接任意球和
球门球共用的 `GoToBallWaitAndKick:wait`，以及 `PassToTeammate` 的等待点和等待状态。
主罚队员直接进入传球或踢球请求，由底层完成走近与对齐；有接球队员时仍评估传球目标，
没有有效接球队员时使用原来的清球/带球回退。
本方直接开球的 2 秒扫视、点球主罚的 5 秒初始扫视和点球接近后的 4 秒准备扫视也跳过。
这些分支仅在 RCSS 构建且 `--localization truth` 时启用；`measured`、实机及原版 SimRobot
保留原流程。裁判 Initial/Ready/Set、对方开球等待、处罚/急停和确实丢球后的搜索仍生效。

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

7v7 的队内消息定义放在 `Config/Locations/RCSS_fifa7vs7/teamMessage.def`，
truth/measured、K1/T1 共用。绝对位置范围为每轴 ±32.768 m；机器人相对的球位置、
行走目标和出球目标为每轴 ±131.072 m，覆盖整块 61 × 42 m 草坪及任意朝向下的跨场目标。
相应增加编码位数，坐标量化步长约 8 mm；坐标扩展本身增加 2～3 字节，
当前新增阵型还使 proposedTactic、acceptedTactic 各多占 1 位，最终包长按字节向上取整。
原版 `Config/Scenarios/Default/teamMessage.def` 不变。

RCSS 客户端消息版本为 74，新增阵型使 tactic 枚举从 3 位增为 4 位；版本 73 的旧
大场客户端和版本 72 的原始客户端均会被拒绝，避免按错误位宽解码。原版构建的版本号仍为 72。
更新后需**重新编译并重启整队所有客户端**，不能混用更新前的进程。启动参数不变，
server 无需为此修改。此修复解决队友坐标超出 ±12.8 m 后被截断、引发错误球位置分歧的问题；
主罚角色仍使用原版选择算法，truth 模式下的等待策略见上文。

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
| 起身 | 默认 `Config/NeuralNets/GetUpEngine/` 的 K1 模型；Dancer 模式使用 `Config/NeuralNets/Dancer/recovery_mjlab/model_4999.onnx` |

踢球模型的观测、推理与切换条件由原版 WalkingEngine 执行。新增推理记录回调仅在
`BUILD_RCSS_SERVER=ON` 构建中启用，不改模型输入输出。CSV 的 `policy`、
`walk_inferences`、`kick_inferences` 记录最近实际调用的模型和 Walk/Kick 累计推理数；
`policy` 现在也能记录 GetUp 网络，其推理不计入 `kick_inferences`。
`tactic`、`set_play` 记录原生策略选择。
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

2026-10-17：truth 模式定位球等待修改完成后，Release 编译及协议/IMU/队内消息三项测试通过。
7 台原生客户端的独立场景覆盖边线球、角球、两类任意球、球门球、点球和本方开球，
均实际进入 Kick 推理，未执行上述等待状态；对方开球场景未执行 Kick。
按最近一场主罚附近位姿构造的对照中，首次 Kick 推理从约 6.28 秒提前至 4.00 秒，
两层等待均消失；这是受控场景结果，不是原比赛完整状态重放。
单独的 measured 模式开球检查仍能看到原有站立扫视。
场景结果位于 `/tmp/bhuman-restart-matrix-mz8s0n4t/`、`/tmp/bhuman-restart-matrix-finksgpo/`，
前后对照位于 `/tmp/bhuman-truth-waits-jtugkkum/`。

2026-10-17：新增原生队内消息编码回归，协议/IMU/队内消息三项测试通过。
覆盖场边和草坪角落的位置、跨场相对球/行走/出球目标、K1/T1 与 truth/measured 的配置选择，
以及旧版本拒收。原定义重现 `(-20.35, -18.22) m` 被截成 `(-12.8, -12.8) m`、
误差约 9.29 m；新定义测试点的最大二维位置误差约 5.7 mm。
另用最近比赛边线球开始时的 7 人位置进行独立 server 对照：旧定义稳定阶段同时有
5～6 个 `playBall`，新定义中 4 号恢复防守、6 号持续主罚。
部分远处队员仍会因球估计波动短暂重选角色，本次没有放宽原版球位置分歧判断。
该实验恢复了起始位姿，并非原比赛的完整状态重放；结果位于 `/tmp/bhuman-team-range-y7fxd7w4/`。

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
