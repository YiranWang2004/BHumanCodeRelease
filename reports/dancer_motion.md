# AMP velocity / 等变 VisualKick 接入记录

本报告记录先接入 AMP / VisualKick、仍保留原起身时的实验。后续已将 Dancer 模式的
起身替换为 `model_4999.onnx`，包括起身期间的头部接管；最新验证见
[起身策略接入报告](dancer_recovery.md)。下文原起身实验和 7v7 短联调为替换前结果。

日期：2026-10-25。B-Human 分支：`feature/rcssservermj-7v7`；服务器：
`rcssservermj` 的 `feature/bhuman-7v7`，`7b6b102ac46e15e6838ca706a576c01da797791e`。
来源：`dancer-robocupdemo/test/7vs7`，`aba39596950847771d9ab558a00afab995ff8335`。

## 已确认的实现范围

新增 `--motion-policy native|dancer`，默认 native。dancer 仅用于 K1，继续注册
K1BHuman；服务器模型、球、地面、2 ms 物理步长及 20 ms 网络周期不变。
完整运行资源随 B-Human 仓库保存，SHA256 见
`Config/NeuralNets/Dancer/manifest.json`。

AMP 与 VisualKick 都输出 serial 顺序的 20 个身体动作；头部始终保留 B-Human 控制。
B-Human 的 MotionRequest、行走目标与路径规划、裁判和主罚权、摔倒和起身流程继续使用。
仅 RCSS 构建插入网络替换接口；native 模式和 T1 没有新控制分支。

AMP 使用 69 维单帧输入、原始上一动作反馈，不做动作裁剪或额外滤波；身体目标是
`default + action × action_scale`。速度由 B-Human 现有规划提供，再按 ±2、±1.75、±3
逐轴限幅。站立传零速度。起身后清空 AMP 状态。

VisualKick 使用来源的 50×75 历史、零历史启动、20 个裁剪到 ±1 的动作，固定
command `(1,0,1)`、步频 1.8 Hz。它按 B-Human 的战术方向执行，方向转换包含
Cognition→Motion 里程计旋转和水平坐标→完整机身坐标的倾斜变换。
不把旧 Kick 的 strong、inaccurate、allow_deviation 或目标球速填入不对应的观测槽。

网络使用当帧相机检测转换得到的身体坐标球位置；检测间隔和失去检测时保留上次测量，
不进行外推或重投影。BallModel 仅参与上层决策及退出距离判断，未替代网络视觉输入。

接管条件：当前看见球、身体 XY 球距 ≤6 m、朝球偏角绝对值 ≤0.65 rad，且原生
行走阶段允许处理球。没有额外进入等待。退出条件包含原生停止、避障路径、截球/让球，
以及 >1 s 未见球或 BallModel 距离 >8.5 m 持续 ≥600 ms；退出后冷却 1200 ms。
不添加 dancer 的无进展或总执行时间限制。非行走阶段打断后清空网络历史；正常的
walk/stand 阶段交替不反复清空 AMP。一次网络帧至多推理一次。

## K1BHuman 模型适配

K1BHuman XML 手臂含非零 `ref` 和静态旋转，不能只看 `ref` 就再次添加偏置。
在两个真实 MuJoCo 模型上给定相同 serial 关节角，其对应身体旋转最大差为
`3.962×10⁻¹³`，因此传感器和命令采用相同关节角约定。身体几何和惯量仍有差异，
不能把这个检查理解成两个模型的动力学相同。

相机使用 K1BHuman 的实际结构：头部旋转原点 `(0.0056,0,0.2479)` m，
相机 site `(0.0601,0.0351,0.0928)` m；不沿用标准 K1 的无横向偏移相机。
100 个随机机身姿态、头部角度和球位中，C++ 球转换相对 MuJoCo 正向运动学的
最大误差 `3.553×10⁻¹⁵` m。

身体 Kp/Kd 取模型配置；B-Human 头部增益保持原值。关节限位、延迟、速度滤波和
力矩上限仍由现有 K1BHuman 与原控制链执行。特别是腿部上限保持
50/50/30/60/30/30 N·m，并未提升为训练模型的 68/76/38.3/112/38.3/38.3。

## 验证结果

5 项 CTest 全部通过：协议、IMU、队内消息、大场阵型、Dancer 接管门限。
接管测试覆盖进入距离与角度边界、短时丢帧、1 s 丢球退出、600 ms 距离确认、
计时器清除、1200 ms 冷却及原生控制优先级。

数值对照直接调用来源 Python AMP 实现及来源 C++ VisualKick 部署探针，分别运行
200 帧，覆盖多次 reset、长时丢球、零测量、速度限幅、头部变化和动作历史反馈。
观测、动作、身体目标及增益的最大绝对差分别为 `2.623×10⁻⁶` 和 `1.490×10⁻⁶`。
这里验证移植一致性；未声称重新完成训练质量评估。

所有动态实验使用独立 TCP 端口和队内通信 ID，只清理实验创建的进程。
结构化摘要见 [dancer_motion_results.json](dancer_motion_results.json)，原始文件在忽略的
`logs/` 下。原服务器 `playMode` monitor 命令只写日志、不会改变状态，因此首轮
未进入 PlayOn 的走路尝试 `dancer-motion-w_ue1rot` 已剔除；正式走路测试使用 `dropBall`。

### 行走与起身

`dancer-motion-7jzkqgjf`：两种策略分别执行相同的零速、前进、侧移和旋转请求，
有效 PlayOn 时长约 9.2～9.9 s，全部正常退出且无摔倒。
这是保留原行走规划后的速度请求测试；例如 `--test-cmd` 的角速度还会经过原生
WalkAtSpeedEngine 的缩放，不能把 CLI 值直接当作最终网络输入。

| 请求 | 原策略实际位移 | Dancer 实际位移 |
| --- | --- | --- |
| 前进 0.8 | 9.79 s 内 x +7.85 m | 9.79 s 内 x +6.538 m、y −1.127 m |
| 侧移 0.4 | 9.23 s 内 y +3.683 m | 9.63 s 内 y +1.177 m |
| 旋转 0.6 | 9.61 s 内 yaw +1.418 rad | 9.63 s 内 yaw +0.246 rad |

这表明新模型在 K1BHuman 上的侧走和小角速度响应较弱，不能以“部署数值相符”推断
跟踪性能相同；本次没有擅自调大速度或修改模型物理参数补偿。

`dancer-motion-bozfqeif`：改用真正的 stand 请求，人为令机器人前倾 1.4 rad，
两种模式均执行原生 fall/getUp 并恢复站立。Dancer 最终恢复 AMP，根高度 0.548 m；
native 最终高度 0.545 m。全过程各 1800 帧，客户端没有退出。

### 踢球

使用现有 7 人定位球实验框架，仅 4 号自由行动，其余 6 人为原生站立夹具。
计时前固定 4 号根位置并指向球，正式计时后解除固定，运行完整视觉、行为与物理。
这不是原比赛关节状态回放，也不是配对多随机种子的统计胜率实验。

| 起点／模式 | 开始 Kick | 球速首次 ≥1 m/s | 其后 120 ms 球速 | 相对战术方向误差 |
| --- | ---: | ---: | ---: | ---: |
| 3 m、正对球／Dancer | 0.27 s | 2.62 s | 3.098 m/s | 2.15° |
| 6 m、正对球／Dancer 首轮 | 0.53 s | 4.04 s | 3.131 m/s | 7.70° |
| 3 m、初始偏转 60°／native | 1.75 s | 2.54 s | 1.379 m/s | 31.73° |
| 3 m、初始偏转 60°／Dancer | 0.61 s | 2.66 s | 3.557 m/s | 0.26° |
| 6 m、正对球／native | 3.91 s | 4.92 s | 5.250 m/s | 6.91° |
| 6 m、正对球／Dancer 对照轮 | 0.36 s | 3.86 s | 3.201 m/s | 6.17° |

各场景观察 12 s，均无摔倒、无处罚，记录到脚部接触。
数据来自 `kick-alignment-x5514m1o` 和 `kick-alignment-o_fh88ab`。
新策略实现了远处提前接管，但并非每种起点都更早出球，6 m 对照的球速也低于原策略。

### 完整 7v7 短联调

`dancer-team-rhfkb8v6`：双方均启用 Dancer，正常左队开球，无固定站立夹具。
14 台全部连接，每台收到 6 名队友信息；每台记录约 26 s、1300 帧的非 BeforeKickOff
过程，随后进入 PlayOn；6 台曾实际执行 VisualKick。启动器正常结束，没有客户端崩溃。

蓝 6 号在比赛中摔倒并进入原生起身，实验结束于 server time=28.03 s 时仍在起身，
不能将其写成“比赛中已经成功起身”。上面的独立倾倒实验验证了恢复链路，
本次短联调不证明整场稳定性，也未与 Apollo 实际对赛或测量胜率。

## 复现

```bash
cmake --build build_sim --target run_player rcss_dancer_policy_probe rcss_dancer_gate_test -j4
ctest --test-dir build_sim --output-on-failure
../booster_mjlab/.venv/bin/python scripts/experiments/dancer_policy_parity.py
/home/robotics/miniconda3/envs/robocup_sim/bin/python scripts/experiments/dancer_model_contract.py
python scripts/experiments/dancer_motion_smoke.py \
  --server-python /home/robotics/miniconda3/envs/robocup_sim/bin/python
python scripts/experiments/dancer_motion_smoke.py \
  --server-python /home/robotics/miniconda3/envs/robocup_sim/bin/python --commands 0,0,0 --tip
/home/robotics/miniconda3/envs/robocup_sim/bin/python scripts/experiments/kick_alignment.py \
  --baseline build_sim/run_player --candidate build_sim/run_player \
  --candidate-motion-policy dancer --modes baseline,direct \
  --cases 3_0_60,6_0_0 --seconds 12 --team-id 94
```

数值对照需要 NumPy、ONNX Runtime 及来源仓库已编译的 `fast_student_deploy_probe`；
模型几何和仿真实验需要 server 的 MuJoCo 环境。正常运行 B-Human 不需要这些验证依赖。
7v7 联调的具体启动参数保存在该次日志目录的 `manifest.json` 中；复现时使用空闲
端口，并给测试球队分配与其他比赛不同的 team-id。
