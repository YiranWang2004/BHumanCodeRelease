# 可选 7v7 阵型验证

日期：2026-10-17。实现位于 `feature/rcssservermj-7v7`。

## 约定与实现

- 默认 `--strategy current`，保持现有 `s7v7` / `t231`。
- `balanced` 对应新增 `balanced7v7`：`t222 ↔ t123`。
- `attacking` 对应新增 `attacking7v7`：`t222 ↔ t033`。
- 使用 2025 的场上人数条件、中线 ±1 m、持续 3000 ms，以及各阵型减员时位置优先级。
- 场上基准点取自 `coderelease2025` 默认 9×6 m 场地，x、y 分别按当前场长、场宽归一化。
  旧 `forwardM` 映射为今年已有的 `forwardC`，不新增角色。朝向由今年的角色逻辑计算。
- t033 使用今年的普通门将，基准点前移至当前己方罚球点；没有移植旧 attackingGoalkeeper。
  常规比赛中的门将目标通常仍由今年的球门防守计算决定。
- 三套策略引用相同的本分支 7v7 定位球配置；未修改任何已有定位球文件。
- 原生 `Behavior.cpp`、位置角色、踢球决策、走路及 policy 实现均未修改。
  RCSS 模块构造时根据显式参数选择策略；原生状态机继续处理切换和队内投票。
- tactic 枚举扩大，RCSS 消息版本升为 74，拒收 72、73 版本；整队必须重新启动为新客户端。

## 原生行为与通信测试

```bash
cmake --build build_sim --target run_player rcss_formation_test rcss_team_message_test -j4
ctest --test-dir build_sim --output-on-failure
```

结果：4/4 通过（protocol、imu、team_message、formation）。

`rcss_formation_test` 在正常框架 globals、配置搜索路径和 Blackboard 中构造真实
`Behavior`，输入确定的球位置及队员记录，执行其 `preProcess/update/postProcess`。
没有在测试中重写切换算法。K1/T1 × truth/measured 四种配置均覆盖：

1. 球在 +0.999 m 保持超过 3 秒，不转入进攻阵型。
2. 球在 +1.001 m：2980 ms 时不切换，3000 ms 时切换。
3. 球回到 x=0 时保持当前进攻阵型。
4. 球在 −1.001 m：2980 ms 时不退回，3000 ms 时退回 t222。
5. 5 名场上队员可以进入 t033，不能由 t222 进入 t123；已在进攻阵型时人数为 5 不立即退出。
6. 场上队员降至 4 时立即退回 t222；继续减至只剩门将，位置分配仍有效。
7. 所有被分配的位置唯一，基准点有限，Voronoi 区域非空。
8. 三种新阵型的全部场上基准点逐一核对 2025 参考值的归一化结果，误差小于 0.1 mm；
   门将基准点分别为 −23.65 m、−23.65 m、−20.18 m。
9. 自方/对方开球和点球准备阶段，仍选择当前 7v7 定位球及 t231。
10. current 在球位于己方和对方半场时都保持 t231。

测试只让被测机器人提交阵型投票，其他队员不提交虚构票数；因此验证的是原生切换、
分配与单机决策，不代表已验证真实七进程的投票收敛时间。

`rcss_team_message_test` 用真实压缩编解码器逐一往返全部新旧 tactic 值，验证
proposed/accepted tactic 与后续镜像、位置字段；同时保留大场坐标负对照，并验证
当前版本可接收、旧 72/73 版本被拒绝。

## 独立 server 启动验证

使用本地 `rcssservermj`、robocup_sim 环境，在临时 TCP 端口分别运行三个独立无界面
server；UDP 队号为 95、96、97。没有连接、停止或重启用户的比赛。
测试 server 每帧保持 PlayOn，单个真实客户端执行 native stand，使用正常视觉、
GameController、行为与 policy 调度；其余队员由现有行为模块的缺失消息后备记录表示。

| 启动参数 | 记录帧数 | 有效阵型帧数 | 实际阵型 |
| --- | ---: | ---: | --- |
| K1 / truth，不加 strategy | 450 | 434 | t231 |
| K1 / truth / balanced | 450 | 430 | t222 |
| T1 / measured / attacking | 450 | 428 | t222 |

三次客户端均正常退出；验证了实际启动参数能够传入模块，默认方案保持原阵型。
动态切换边界由上述真实 Behavior 测试验证；启动测试本身不证明对抗表现改善，
也没有验证门将实际前移。

本机原始 CSV、stdout、server fixture 与结果保存在被忽略的
`logs/formation-smoke-r0amlxgb/`，不作为项目运行依赖。
