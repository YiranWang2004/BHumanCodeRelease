# Dancer Walk / VisualKick 过渡对齐

## 范围与结论

本次修改仅用于 `--motion-policy dancer`。已移除 Dancer 适配中的
`native_motion_guard`：B-Human 的瞬时截球、让球、避障、普通走路或站立请求不再
直接结束正在执行的 Kick 会话。默认 native 模式不变。

过渡规则依据本机 `dancer-robocupdemo` 当前 `test/7vs7` 分支，提交
`aba39596950847771d9ab558a00afab995ff8335`：

- `src/brain/include/student_visual_kick_entry.h`
- `src/brain/src/brain_tree.cpp`：`RLVisionKick::onStart/onRunning`、`visualKickBallUsable`。
- `src/brain/src/brain.cpp`：视觉检测/记忆有效期。
- `src/brain/config/config.yaml` 及 `subtree_striker_play.xml`：退出准备、执行时限。

B-Human 仍负责初次处理球请求、球队主罚权、裁判约束、战术踢球方向和头控。
球队许可取策略层处理球技能或 `playBall/kickOffForward` 角色，不取会随截球/避障变化的
底层 MotionRequest。未复制 dancer 的另一套球队 cost 仲裁、守门员专用球源或头控。
球估计使用 B-Human BallModel；网络视觉输入仍是 server 相机测量，未引入全局球真值。

## 实际规则

1. 有 B-Human 初次处理球请求和球队许可、当前可见球 ≤6 m、偏角 ≤0.65 rad，
   进入 300 ms AMP 零速准备。期间再次检查几何、视觉和许可；条件失效则取消并冷却。
2. 启动 Kick 后保持会话。非处理球运动请求不会覆盖已有的场地坐标踢球方向。
3. 正常退出条件：失去球队许可；无可用球源持续 1000 ms；球距 >0.10 m 且
   球 x<−0.10 m 或 |偏角|>1.75 rad 持续 600 ms；球距 >8.5 m 持续 600 ms
   且本轮 >1500 ms；5 秒无进展；会话满 14 秒；7 秒一轮最多延长一次且须有可用球源。
4. 进展阈值与源实现一致：球距缩短 0.12 m、球沿原踢球方向前进 0.15 m、
   机器人移动 0.20 m、转向 0.25 rad，满足任一即更新基线和进展时间。
5. 正常退出先准备 140 ms，再切 AMP 零速；下一控制帧恢复规划速度。
   1200 ms 冷却从实际恢复 AMP 起计。退出准备期间 Kick 仍在运行，其固定 command
   不会因 Walk 的零速指令而变小。
6. 裁判禁止 Kick 和摔倒/起身/其他非行走运动接管立即中断。禁止 Kick 本身不会
   禁止合法的 AMP 防守走位。只在真实切换或运动中断后重置模型历史。

视觉每 40 ms 一帧，推理每 20 ms 一次；无新 See 消息的控制帧保持上次检测状态。
检测新鲜期为 650 ms，球记忆最多 1000 ms；未检测到且距离 ≤0.45 m 不采用记忆。
再连续 1000 ms 无可用源才正常退出，不能将其理解为“最后看到球后固定 1 秒退出”。

## 验证

CMake 编译 `run_player`、状态机测试和真实 ONNX 适配器测试通过。
CTest 共 6 项通过。新增检查覆盖：

- 进入准备、取消、退出准备、冷却的精确边界。
- 丢球、坏球姿态、距离最小时长、四种进展、无进展和 14 秒时限。
- 40 ms 视觉 / 20 ms 控制的交错节奏，以及每网络帧只推理一次。
- 真实 AMP / VisualKick 接口下，截球/让球/避障/停止/站立各标记均不能中断会话。
- 非处理球请求不能修改锁存方向；球队许可撤回按正常流程退出。
- 裁判停止立即退出、摔倒中断立即记录，以及禁止 Kick 时合法 AMP 行走仍可执行。

物理实验使用当前临时普通 K1 模型、fifa7vs7、SimRobot 对齐的接触参数及 0.41 kg 球；
每次使用独立端口和球队编号。七人场景中仅 4 号自主运动，其余六人作为站立队友。
实验启动前固定根姿态并建立新鲜球观测，然后放开全部物理。它们是受控场景，
不是原比赛的关节状态重放，也不是完整双方 7v7 的性能统计。

| 场景 | 准备阶段 | 截球标记触发时仍执行 Kick | 12 秒内摔倒 | 球离脚后 120 ms 速度 / 方向偏差 |
|---|---|---|---|---|
| 静止球，初始距离 3 m | 0.30 s | 此场景未触发 | 无 | 3.50 m/s / 2.51° |
| 迎面球，初始距离 5.8 m、初速 2 m/s，第 1 次 | 0.30 s | 27 帧，0.54 s | 无 | 4.00 m/s / 28.38° |
| 同上，第 2 次，初始朝向扰动 3°、x 扰动 0.02 m | 0.30 s | 14 帧，0.28 s | 无 | 3.46 m/s / 37.83° |

第 1 次迎面来球的日志明确记录：主罚权撤回后继续 Kick 140 ms，再恢复 AMP。
两次迎面来球都观察到真正的足部触球。第 2 次超过原实验 30° 的方向误差门限，
因此射门成功判据为失败；不能将本次“过渡规则通过”解释为踢球精度已解决。
迎面球已有初速度，实验脚本已将其出球判定改为机器人首次接触之后，避免把开场滚动误算作踢出。

额外 AMP 行走检查：600 帧，活动阶段 9.75 s，前进 7.216 m，最低根高度 0.518 m，
未进入摔倒/起身阶段。验证禁止 Kick 不会意外压掉正常行走速度。

原始记录：

- 静止球：`logs/kick-alignment-0txnye8r/`
- 两次迎面球：`logs/kick-alignment-4_z5v0eo/`（含 manifest、CSV、完整接触/速度数据、JSON 指标）
- AMP 行走：`logs/dancer-motion-xlq_90_8/`
- 首次探索迎面球：`logs/kick-alignment-kynkf852/`，其中旧版自动报告将初始滚动误作出球，
  其射门指标不采用；原始 CSV 仍能证明截球标记期间 24 帧连续 Kick。

## 复现

```bash
cd /home/robotics/ZJUDancer/BHumanCodeRelease
cmake --build build_sim --target run_player rcss_dancer_gate_test rcss_dancer_adapter_test -j4
ctest --test-dir build_sim --output-on-failure
python3 scripts/experiments/kick_alignment.py \
  --server-python /home/robotics/miniconda3/envs/robocup_sim/bin/python \
  --baseline build_sim/run_player --candidate build_sim/run_player \
  --candidate-motion-policy dancer --modes direct \
  --cases 5.8_0_0 --incoming-speed 2 --seconds 12 --repeats 2 --team-id 95
```

比赛启动参数不变。使用 `--log` 时新增 `dancer_exit_reason`、`dancer_ball_owner`、
`dancer_game_allowed`、`dancer_native_flags`、`dancer_kick_direction_world`，
可区分原始运动约束、真正退出原因和实际锁存方向。`dancer_gate` 显示准备/运行/冷却阶段。
