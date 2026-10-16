# B-Human 实时桌面监视器

此工具用于本分支的 rcssservermj 客户端，支持 K1、T1 和远程监视。
界面使用 SFML 原生桌面窗口；它被动接收状态，不控制机器人或裁判。
第一版仅实时查看，没有离线回放，也不自动保存任何比赛数据。

## 编译与运行

在仓库根目录执行：

```bash
# Ubuntu 首次安装依赖；比赛电脑只运行 run_player 时无需安装 SFML
sudo apt install build-essential cmake libsfml-dev libjsoncpp-dev fonts-noto-cjk
scripts/build_monitor.sh
scripts/start_monitor.sh
```

构建输出位于已忽略的 `build_monitor/`，与 `build_sim/` 分开。
当前客户端源码也需重新编译一次：`cmake --build build_sim --target run_player -j4`。

服务器照常启动后，在另一个终端执行：

```bash
# K1
for n in {1..7}; do ./build_sim/run_player --num "$n" --monitor & done
# T1 将上述 run_player 参数添加 --robot T1
```

客户端默认不发送监视数据，只有 `--monitor` 开启。与 `--log` 独立：
只加 `--monitor` 不写 CSV；同时加 `--log` 才额外保存原来的每帧日志。
监视器可以先启动或后启动，关闭窗口不会停止机器人。

## 远程监视

监视电脑编译本目录后：

```bash
scripts/start_monitor.sh --listen 0.0.0.0 --port 10190
```

比赛电脑将目标设为监视电脑的 IP，例如：

```bash
for n in {1..7}; do
  ./build_sim/run_player --num "$n" --monitor --monitor-host 192.168.1.50 --monitor-port 10190 &
done
```

需要能从比赛电脑向监视电脑发送指定端口的 UDP。此端口是遥测接收端口，
与服务器 agent TCP `60000`、裁判 TCP `60001` 无关。
一个客户端发送到一个目标；一个监视器可以发现多个队伍。
`--team BHuman` 可按队名过滤界面；`--font /path/font.ttf` 可指定中文字体。
不同比赛使用不同接收端口最方便；程序也按客户端报告的 server 地址、team ID、队名区分球队。

## 界面与字段

- 左侧同时显示 1–7 号的机型、角色、主要活动行为、当前采样帧推理网络和接收/见球间隔。
- 球场使用客户端 `FieldDimensions` 的实际线段和尺寸。本队机器人显示各自使用的定位值；
  坐标统一为本队向右进攻（+X），与服务器绝对左右场不同。红点是选中机器人的
  `GlobalOpponentsModel`，橙点是它的球估计，均不是监视器直接查询 server 获得的全场真值。
- 右侧区分 `SkillRequest`、`MotionRequest`、实际 `MotionInfo.executedPhase` 和真实网络推理。
  `walkToBallAndKick` 是动作请求；`walk` phase 也可能执行 Kick 网络。
  “本帧推理网络”只显示该 server 帧内实际调用的模型，覆盖 Walk、Kick、GetUp。
  若本帧没有新推理，显示“无新推理采样”；底层仍可能保持上一次网络输出，
  可结合实际 phase 与“最近网络 / 间隔”判断。不会把上一轮 Kick 误报为本帧推理。
- “完整活动行为树”包含该帧 `ActivationGraph` 的所有活动节点及其层级、状态、
  option/state 持续时间和参数；不包含没有被调用的分支。滚轮可以查看屏幕以外的节点。
- 时间线显示角色、主要行为、实际推理网络、受罚状态的变化与客户端重启。
  `play` 是 server 的比赛累计计时 `GS(t)`，`sim` 是仿真累计时间，均不是半场倒计时。
  每台机器人在内存中保留最近 200 条，关闭后丢弃。

点击卡片或球场机器人，或按 `1`–`7` 选人；`Tab` / `Shift+Tab` 或球队栏左右箭头切换球队。
在时间线和行为树区域使用滚轮；被截断的文本可悬停展开；`Esc` 退出。

遥测按墙钟最多 10 Hz 发送完整快照，UDP 不重传。时间线反映收到的采样变化，
小于采样间隔的短暂状态可能没有出现；需要逐帧分析时仍用独立的 `--log`。
接收间隔超过 0.5 秒标记延迟，超过 2.5 秒显示“未更新”并将球场机器人变灰，
保留最后快照供查看。暂停仿真、断网、客户端退出都可能导致未更新。
程序会丢弃格式错误、重复和乱序包；客户端重启后允许新会话重新从小序号开始。

## 验证

```bash
ctest --test-dir build_monitor --output-on-failure
```

模型测试覆盖数据格式、乱序、重启、过期网络显示和时间线容量。
本机还用独立 server 和 7 台原生客户端（含 K1、T1）验证了桌面显示、行为树、
场地尺寸、实际 Walk/Kick/GetUp 推理，以及默认关闭发送和无效包处理。

无桌面环境的接收诊断可以显式运行：

```bash
build_monitor/bhuman_monitor --headless --duration 10 --port 10190 --summary /tmp/monitor-summary.json
```

`--summary` 仅在显式指定时写最终快照及接收计数，用于诊断，不是回放日志。
桌面诊断支持 `--frames N --screenshot /tmp/monitor.png`，在指定帧数后截图退出。
