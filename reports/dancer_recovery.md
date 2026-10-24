# Dancer 起身策略接入与验证

日期：2026-10-25。B-Human 分支 `feature/rcssservermj-7v7`。
仅 `--motion-policy dancer` 的 K1 使用新起身，默认 native 和 T1 保留原实现。
已编译当前 `build_sim/run_player`；模型、配置及部署代码均在 B-Human 仓库内，
运行不依赖旁边的 dancer 仓库或 Python 进程。

## 来源与执行语义

来源 `dancer-robocupdemo/test/7vs7`，提交
`aba39596950847771d9ab558a00afab995ff8335`，原始部署为 `getup_mjlab.py` 与
`sim_worker.py`。使用 `recovery_mjlab/model_4999.onnx`，SHA256：
`0f5eec629d9f3b22600f1378e8f5f6518f9280facbf8ab617bef28cf6ed87852`。
模型和 JSON 与来源逐字节核对，资源校验清单见 `Config/NeuralNets/Dancer/manifest.json`。

每 20 ms 一次推理。72 维输入依次是机身角速度 3、机身坐标重力 3、
22 关节角减默认角、22 关节速度、上一帧原始动作 22。关节顺序采用 K1 serial
顺序，包含头部；使用 server 当帧传感器值，上一动作初始为零。

22 维输出是原始动作，不能当作持有 20 ms 的普通位置目标：训练的目标是每个
物理子步当前关节角加 `0.6 × action`。因此移植输出 `Kp=0`、配置 Kd 和
`tau=训练Kp × 0.6 × action`，由 server 每个物理子步施加阻尼，并限制最终合力矩。
不单独裁剪前馈力矩，不增加手臂零位偏置，不提高 K1BHuman 原有限位或力矩上限。

- 前 20 帧（0.4 秒）保持阻尼、前馈力矩为零，但网络继续更新上一动作。
- 沿用用户确认的 dancer 实际调度条件：`gravity_z < -0.85`，且根高度 ≥0.30 m。
  当帧仍发送起身命令，下一网络帧交还 AMP，由 AMP 完成最后站直。
- 300 帧（6 秒）未完成，释放力矩和阻尼一帧，重置历史并重新起身。
- 起身时接管全部 22 个关节，包括头部；进入 Walk/Stand 后恢复 B-Human 头控。
- B-Human 的摔倒保护、进入 GetUp 的判断、playDead/prepare 优先级保留。
  未额外移植 dancer 自己的摔倒检测器。退出起身后清空 AMP/Kick 历史。

起身网络的部署类内部还有“高度 >0.53 m 且接近直立连续 50 帧”的成功条件。
原 dancer 调度器实际上提前按上述 0.30 m / 重力条件交还 AMP；本实现也如此，
没有额外等待那 50 帧。

## 验证结果

服务器为 `rcssservermj/feature/bhuman-7v7`，提交
`7b6b102ac46e15e6838ca706a576c01da797791e`。全部物理实验使用 fifa7vs7、
`--bhuman-physics`、K1BHuman、2 ms 物理子步和 20 ms 控制周期，保留服务器当前球、
地面、电机速度滤波和力矩上限。网络实验使用独立端口，只清理本次创建的进程。
结构化数据见 [dancer_recovery_results.json](dancer_recovery_results.json)。

### 逐帧部署对照

C++ 与来源 Python 实际 actor 对照 540 帧，包含 20 帧落稳、动作历史、重置、
300 帧超时、连续 50 帧成功、终止后的重复调用。观测、动作、力矩和 Kd
的最大绝对差均为 0，状态和帧计数一致。

编译通过，5 项 CTest 全通过。原 AMP/VisualKick 各 200 帧对照仍通过，
最大绝对差分别为 `2.623e-6` 和 `1.490e-6`。

### 原始 Python 策略的物理可行性

直接使用来源未经修改的 Recovery 和 AMP actor，驱动 server 编译出的真实场地、
机器人及电机控制器；没有网络、裁判或 B-Human 行为。四种倒地方向各测两次，
第二次改变偏航和关节扰动。起身计时前先自由落地 0.8 秒，无根位置固定。

**8/8 成功，全部第一次起身成功。** 从首次 Recovery 控制到交还 AMP 为
1.18～1.42 秒；交还后要求连续 5 秒高度 >0.45 m 且 `gravity_z < -0.85`。
日志在 `logs/dancer-getup-physics-t4rytrrn/`。

### 完整 B-Human/server 链路

正常启动 B-Human，通过 monitor `move3d` 将正在站立的机器人向指定方向倾斜
1.4 rad，然后释放，由原生摔倒检测与 Fall phase 决定起身时机。
以下时间均从 **GetUp policy 首帧** 起算，不包含前面的摔倒保护等待。

| 场景 | 交还 AMP | 起身尝试 | 交还后观察 | 观察期最低高度 |
| --- | ---: | ---: | ---: | ---: |
| 前倒 | 1.20 s | 1 | 28.70 s | 0.538 m |
| 后倒 | 1.22 s | 1 | 28.38 s | 0.533 m |
| 左侧倒 | 1.26 s | 1 | 28.38 s | 0.534 m |
| 右侧倒 | 1.34 s | 1 | 28.40 s | 0.538 m |
| 前倒复测，含最终日志状态修改 | 1.20 s | 1 | 28.30 s | 0.539 m |

五次均实际调用 `model_4999.onnx`，之后持续调用 AMP，无再次进入 Fall/GetUp，
最后连续 5 秒高度均 >0.45 m。最终前倒复测额外通过重力投影直立检查。
原始目录依次为 `dancer-motion-ni_0v6wo`、`gp5e408e`、`wy7gl31r`、`cmwo6zb6`、
`mygkixl1`，均位于 `logs/`，后四个同样带 `dancer-motion-` 前缀。

另以持续前进请求 `--test-cmd 0.8,0,0` 测试行走中前倒：1.22 秒交还 AMP，
随后继续行走约 28.6 秒，无再次摔倒，最后 5 秒最低高度 0.520 m，
直立检查通过。原始目录 `logs/dancer-motion-vbfdbe6c/`。

native 回归实验仍调用原 `K1.onnx`，最终站立高度 0.545 m，正常退出；
日志 `logs/dancer-motion-u6paamh6/`。本次没有改变原模式的起身行为。

## 结论与范围

在当前 K1BHuman 模型和电机限制下，新起身可以正常执行并衔接 AMP，已接入
Dancer 可选模式。以上包含 8 次底层物理试验和 6 次完整链路试验，均成功。
尚未统计多人碰撞、被压住或长时间整场比赛中的起身成功率；超时 actor 已做数值测试，
未以卡住身体的物理夹具实测连续重试。不能由这些结果推断实机或整场比赛成功率。

## 运行及复现

```bash
# 比赛客户端：不加 --motion-policy dancer 仍是原模式
for n in {1..7}; do
  ./build_sim/run_player --num "$n" --motion-policy dancer &
done

# 可选 --log 保存 CSV，--monitor 发送监视状态
# 以下为开发验证，需要来源仓库和 server Python 环境
cmake --build build_sim --target run_player rcss_dancer_policy_probe -j4
/home/robotics/miniconda3/envs/robocup_sim/bin/python scripts/experiments/dancer_recovery_parity.py
/home/robotics/miniconda3/envs/robocup_sim/bin/python scripts/experiments/dancer_getup_physics.py
for direction in front back left right; do
  /home/robotics/miniconda3/envs/robocup_sim/bin/python scripts/experiments/dancer_motion_smoke.py \
    --server-python /home/robotics/miniconda3/envs/robocup_sim/bin/python \
    --policies dancer --commands 0,0,0 --tip --tip-direction "$direction"
done
```

CSV 和 monitor 状态包含起身帧数、尝试次数及 `settling/executing/retrying/handoff`。
离开起身后状态变为 `inactive`，保留最近一次的帧数和尝试次数供诊断。
