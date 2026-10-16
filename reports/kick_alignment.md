# K1 近球准备位与 Kick policy 接管实验

日期：2026-10-17。问题针对比赛 3:39.46～3:43.84 的
`GoToBallWaitAndKick:notAtBall / WalkToPoint:walkClose`，不包括已取消的固定扫视等待。
逐次结果见 [CSV](kick_alignment.csv)、[JSON](kick_alignment.json)。

后续采用说明：用户确认后，truth 已统一采用本文 C 方案；以下结论和数据保留为修改前的实验记录，
其中“默认未改变”描述的是实验完成时的状态。当前启动及模式优先级见仓库 README_7v7.zh-CN.md。

## 结论

**Kick policy 确实可以自己接近、绕到球后并踢球，进入它之前不必先满足固定的
“球后 0.5 m、面向球门”姿态。但当前 fast-max command 下直接无条件删除准备位，
会用部分出球精度和力度稳定性换取速度，不能视为无损优化。**

原场景的 5 组对照中，直接接管让首次出球时间中位数从 5.86 s 降到 2.40 s；
原流程 5 次方向误差均不超过 4.47°，直接接管出现一次 20.08° 偏差和一次偏弱出球。
它们说明这段整姿具有实际稳定收益，并非纯粹等待；也说明整段整姿不必完全由 Walk 网络先完成。

补充的“提前接管、保留姿态约束”方案仍请求强踢和最大目标球速，只将
`flag_allow_deviation` 从 1 改为 0。这个场景 5 次的出球时间中位数为 2.66 s、
方向误差最大 7.84°，是值得继续验证的折中方案；不能由这 5 次推出所有场景都可靠。
本轮仅新增诊断和实验材料，**默认比赛流程和 command 均保持原样**。

## 与训练语义的对应

部署文件 `Config/NeuralNets/Walk/K1_kick_inner.onnx` 与
`MachineLearning/IsaacGymRL/pre-trained/WalkAndKick/K1_kick_inner.onnx` 的 SHA-256 完全相同：
`7fb9ea33c36e14332861025f22e92f2f4a7ae7681f4532bdd6e1e78975fe3452`。

`K1_Ball` 继承 `T1_Ball`，具体依据如下（行号对应本次检查的训练仓库）：

- `envs/t1_ball.py:695`：初始静止球距离配置为 0.4～1.5 m，并将球相对机器人方位随机化到整周；
  另有近球重置专门学习绕球，不是假定球始终在已对准的脚前。
- `envs/t1_ball.py:846`：踢球目标方向在 `[-π, π)` 随机采样。
- `envs/t1_ball.py:1520`：59 维输入包含重力、角速度、球的相对位置、目标相对方向、步态相位、
  关节状态、上次动作、球速、目标球速和上次球位置。它没有“上层已经完成整姿”的输入或开关。
- `envs/t1_ball.py:1777`、`:1883`：奖励包括接近球、转向和从侧面绕球。
  学习使用的准备点会随状态由球后约 0.35 m 向球移动，不要求固定停在 0.5 m 处。
- `envs/t1_ball.py:1832`：出球方向奖励始终存在；强踢速度奖励也乘以方向奖励。
  strong 不是无视方向的随意大力踢。
- 输出为 12 个腿部关节目标及 1 个步频量，所以同一个网络可以连续走动、转向和踢球。

三个 command 标志必须按输入顺序理解：

| 标志 | 训练代码名称 | 当前 fast-max 值 | 训练语义 |
|---|---|---:|---|
| 第一位 | `flag_is_strong_kick` | 1 | 启用强踢速度奖励 |
| 第二位 | `flag_is_inaccurate_kick` | 0 | 选择方向奖励的精度参数 |
| 第三位 | `flag_allow_deviation` | 1 | 取消奖励中的额外踢球姿态累积、足底朝向约束 |

部署注释把后两位叫作 `overshoot`、`just-hit`，这些名称不能直接替代训练实现。
这些是训练奖励及学习到的 command 模式的语义，不是在推理时增加或删除一段硬编码整姿限制。
第三位为 1 也不代表删除方向奖励。目标球速输入仍为 3.4 m/s × 0.1 = 0.34；
这是请求值，不是实际物理球速的硬上限。

默认 YAML 是分阶段训练的配置起点；`envs/readme.md` 明确要求训练中途修改多个参数。
因此不把当前 YAML 当作该 checkpoint 最终训练参数的完整记录。本实验直接运行同一份部署网络验证能力。

## 原来 4.38 秒在做什么

`GoToBallWaitAndKick` 先求“面向对方球门中心、位于球后 500 mm”的目标。
本次属于传球请求，`passTarget != -1`，不能使用该 option 中无传球目标时的
300 mm / 20° 快速入口，而要等 `WalkToPoint` 到达：每个位置轴约 100 mm 内、朝向误差约 10° 内。
近目标时还使用 slow 档，K1 配置的最大转速为 54°/s。

比赛 3:40 时，相对这处准备位的朝向误差约 127°，这解释了长时间的近球转身。
到 3:43.82，机器人已走到 y≈18.329 m，即边线外约 0.329 m。
这不是站立等待，而是在用 Walk policy 完成相当严格的准备位约束。

准备位朝向以球门为准，后续传球却按接球队友重新选择方向。重建场景中，两者约为
−40° 与 −88°，存在重复整姿。truth 消除了自身定位需求，却没有消除踢球所需的物理调整。

## 实验设计

- B-Human 行为基线 `fd0866c70`：固定扫视等待已经删除，近球准备位仍保留。
  三组均加入相同 CSV 诊断列，未改变其余推理或物理逻辑。
- server：`rcssservermj`，提交 `a70ae5aa9ae670bc38028abd47995b8bd13742b0`，
  `fifa7vs7 / ssim / sync / bhuman-physics / no-render / no-realtime`。
  物理步长 2 ms、每个客户端周期 10 子步；所有客户端保留原生 10 个 Motion 子帧。
- 每场生成 7 台 K1，4 号正常决策，其余 6 台使用原生 `--test-stand`，保留队内通信和传球目标。
  不加入对方机器人，以隔离整姿问题；没有占用默认比赛端口。
- A：保留准备位 + `(strong,inaccurate,allow_deviation)=(1,0,1)`。
- B：truth 下直接转入原有 pass/kick 分支 + `(1,0,1)`。
  没有强行绕过底层避障、球龄、运动状态和距离判断。
- C：与 B 相同，只将第三个标志改为 0，仍为 strong=1、目标球速 3.4 m/s。
- 原场景采用 3:39.46 的七台机器人根位置/偏航和球位置重建。
  每种方案各 5 次：初始偏移依次为 0、x+20 mm/yaw+3°、x−20 mm/yaw−3°、再重复这两种偏移。
  这不是还原关节角、速度、头部和全部滤波器状态的比赛重放。
- 另外 8 种近球几何，每种 A/B 各 2 次，包括侧向站位、身体偏转、球前反向站位；
  距离 0.7 或 1.1 m。另做 1 次 C 的反向站位/复现脚本验证。合计 **48 次**。
- 计时前固定 4 号根位姿，将头部指向球，使用原有视觉链路获得近球估计；
  要求球龄 <100 ms、估计距离与实际距离差 <0.1 m。事后也核查了完整全局球坐标误差 <0.1 m。
  开始后立即解除固定和头部干预，完全使用正常感知与控制。
  初次发现旧中场球估计影响结果的试跑已排除，不混入统计。
- 奇偶重复交换 A/B 执行顺序。重复包含有限位置扰动和运行调度差异，不能当作独立随机抽样，
  不据此计算总体置信区间或声称统计显著。

K1 正常由 Walk 切入 Kick 网络的初始距离门槛约为 1.2 m，保持门槛约为 1.8 m。
另外还要求球龄不超过 1 s、没有未处理的障碍路径、运动状态适合等。
所以 B 的含义是“提前发出原生踢球请求并让底层判断接管”，不是远距离强制调用神经网络。

## 测量定义

- `first_policy`：CSV 中实际 Kick 推理计数第一次增加，不能用 motion=walk 或上层 option 名代替。
- `launch`：server 记录到水平球速第一次达到 1.0 m/s 的时刻，采样周期 20 ms。
  它不是第一次轻触，也不保证已经踢远。
- `speed_120ms`：首次出球后 120 ms 的真实水平球速；方向也在该时刻取真实球速方向。
- `error_deg`：相对出球前最近客户端帧中 MotionRequest 的世界方向近似值的误差。
  使用真实机器人偏航加请求相对角，不是相对球门方向。
  20 ms 帧间运动、原生里程计补偿会造成小角度测量误差，不把小于约 1° 的差异解释为精度优势。
- 踢球质量同时列出 10°、15°、30° 阈值，并要求出球后球速 ≥1.5 m/s、触球前至出球后
  300 ms 未出现躯干高度 <0.35 m。30° 仅作宽松出球标准，不能称为精确传球。
  10°/15° 也只是比较指标，不代表接球队友能接住；本实验未测目标点到达误差或接球成功率。
- 每次观察 12 s，早于 server 的 15 s 边线球超时；未出球记为右删失，不能解读为永远踢不出。
  所有已出球样本都有 4 号足部接触记录。

## 原场景的 5 次比较

下表时间均为场景开始后的中位数，精度及力度按首次出球测量。

| 方案 | 首次 Kick 推理 / s | 首次出球 / s | 方向误差中位数 / 最大值 | 出球后速度最小值 / m/s | 球速≥1.5 且误差≤10° |
|---|---:|---:|---:|---:|---:|
| A 保留准备位，宽松姿态 | 5.25 | 5.86 | 2.62° / 4.47° | 5.127 | 5/5 |
| B 提前接管，宽松姿态 | 0.33 | 2.40 | 6.10° / 20.08° | 1.216 | 3/5 |
| C 提前接管，保留姿态约束 | 0.31 | 2.66 | 6.52° / 7.84° | 2.539 | 5/5 |

A 的 walkClose 时长为 4.34～5.04 s，与原比赛的 4.38 s 相近。
B 虽然很快开始推理，仍需约 2 s 自己调整；不能把原来整姿时长全部当作能省掉的时间。
B 的偏弱样本球速随后继续下降，而不是 120 ms 测量刚好漏掉紧接着的大力踢。
C 的速度波动也明显，不能把 maximum command 等同于每次实际力度最大。

## 其他几何与实际绕球证据

主 A/B 比较共 21 对：A 在 12 s 内出球 20/21，B 为 21/21；
按“球速≥1.5 m/s 且方向误差≤10°”统计，A 为 20/21，B 为 14/21。
A 已出球样本的最大方向误差为 5.36°，B 为 27.09°。
所有 48 次均无躯干高度低于 0.35 m 或受罚记录。
这进一步说明提速与精度收益不能合并为同一项成功。

一个球前反向站位的 B 样本具有直接证据：以请求出球方向为正，机器人起初位于球前
+0.701 m，触球前移到了球后 −0.097 m；首次出球用时 2.88 s。
从首次 Kick 接管到出球前，共新增 139 次 Kick 网络推理、0 次 Walk 网络推理。
原场景一个 B 样本也从球前 +0.493 m 走到球后 −0.149 m，期间新增 112 次 Kick、0 次 Walk。
因此这些绕球调整确实由 Kick 网络完成，而不是上层标题改变但仍靠 Walk 网络走到位。
反向样本的出球误差为 23.33°，同时说明“能走到球后”并不自动等于“每次踢得准”。

## 建议与适用范围

1. 不把固定球后 0.5 m、朝向球门当作 policy 的必需前置条件；尤其传球方向与球门方向不同，
   先去这一处准备位可能造成重复调整和走到边线外。
2. 不在当前 `(1,0,1)` 下无条件删除并宣称同样准确。直接接管符合尽快解围的偏好，
   对精确传球则有这次已观察到的代价。
3. 下一步优先验证 C 所代表的“提前接管 + 保留 policy 内部姿态约束”，继续保留原生
   远距离寻路、近距离避障、球观测有效性和接管距离门槛。C 目前只验证了 6 次，
   还需要覆盖更多几何及运动球、有对手压迫的场景，才能定为新的默认比赛行为。

本实验仅针对这份 K1 网络、当前 server 物理和 truth 自身定位；没有证明 T1、实机、
自主定位、动态接球队友、拥挤避障或整场胜率也会同样改善。
truth 仍使用原生视觉球估计，并不是把 server 球真值直接喂给 policy。

## 复现与审计

完整 CSV、遥测、server 真值轨迹、场景和三份实验二进制保存在本机：
`logs/kick_alignment_20261017/`（gitignore 忽略）。JSON 中逐条给出了相对路径及二进制哈希。
本报告不依赖把这些大文件提交到远程。

新增 CSV 列 `kick_direction`（弧度）、`kick_type`、`kick_precision` 记录上层请求；
实际网络标志仍由该次使用的变体决定。普通启动仍默认不保存日志。

建议在独立 worktree 编译实验二进制，避免改变正在使用的默认客户端：

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
git worktree add --detach /tmp/bhuman-kick-study 8ccee6131
cmake -S /tmp/bhuman-kick-study/Make/CMake -B /tmp/bhuman-kick-study/build_sim \
  -DBUILD_RCSS_SERVER=ON -DCMAKE_BUILD_TYPE=Release \
  -DRCSS_CXX_COMPILER="$PWD/.hsl-toolchain/bin/clang++"
cmake --build /tmp/bhuman-kick-study/build_sim --target run_player -j4
cp /tmp/bhuman-kick-study/build_sim/run_player /tmp/bhuman-kick-A
git -C /tmp/bhuman-kick-study apply "$PWD/scripts/experiments/kick_alignment_direct.patch"
cmake --build /tmp/bhuman-kick-study/build_sim --target run_player -j4
cp /tmp/bhuman-kick-study/build_sim/run_player /tmp/bhuman-kick-B
git -C /tmp/bhuman-kick-study apply "$PWD/scripts/experiments/kick_alignment_accurate.patch"
cmake --build /tmp/bhuman-kick-study/build_sim --target run_player -j4
cp /tmp/bhuman-kick-study/build_sim/run_player /tmp/bhuman-kick-C
conda activate robocup_sim
python scripts/experiments/kick_alignment.py \
  --baseline /tmp/bhuman-kick-A --candidate /tmp/bhuman-kick-B --accurate /tmp/bhuman-kick-C \
  --modes baseline,direct,accurate --cases replay --repeats 5
```

其他几何用 `--cases '0.7_0_0,0.7_0_60,0.7_0_-60,0.7_60_0,0.7_-60_0,1.1_0_90,0.7_120_90,0.7_180_180'`。
格式依次为距离、接近方位相对球门方向的偏角、身体朝向相对球门方向的偏角，角度单位为度。
`--repeats 2 --modes baseline,direct` 对应主几何矩阵。
同时跑多个实验时必须指定不同的 `--team-id`，避免原生队内 UDP 通信串场。
`kick_alignment_accurate.patch` 是仅用于独立实验构建的消融补丁，不能当作正式实机修改应用。
