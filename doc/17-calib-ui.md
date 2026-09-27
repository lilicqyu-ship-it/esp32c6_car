# 17 · 台架标定 Web UI 需求（编码器判向 · 专用页面 /calib.html）

| 项 | 内容 |
|---|---|
| 文档编号 | 17 |
| 域 | C6 侧（含与 TC275 的跨仓库协议契约） |
| 状态 | V1.1（2026-09-27，追加 §8：逐电机手动控制 · 车辆可视化 · 参数卡片与 TC275 DFlash 持久化 · DPT 命令族 0x71~0x74；V1.0 = 首版判向标定页） |
| 开发者 | AI（本仓库 c6_car 作业；M2 涉及 myCar 侧另立任务） |
| 背景 | myCar F02 闭环伺服（commit 73058cd）落地后，UI 驾驶"只能轻微扭动"。TC275 串口 SRV=/SPD= 台架数据判定：右半桥编码器方向约定反了，右侧闭环成正反馈。TC275 已有 0x70 自动判向（F02 落地），但**无任何用户入口触发、结果只打串口**。 |
| 关联文档 | 本仓库 02-proto / 08-bridge / 14-sf-link / 12-assets-tools；myCar doc/30-tc275/21（§5.2、§15.3）、22（§5.1/§5.4）、23（§8.4）、33（AI 指南） |

> **设计原则：简单、功能完整。** M1 只动 C6，复用既有通路零协议改动即可交付可用台架工具；M2 补一条结果回传帧让结果直接显示在页面上（需要 myCar 固件配合，契约在 §5 定义死）。

---

## 1. 背景与目标

**台架证据（2026-09-27 串口抓取，COM16）：**

```
SRV= 200 76 599 160 -86 656    ← 同一次前向命令：左 duty+599↔实测+76（同号正常），
                                  右 duty+656↔实测-86（反号，闭环在追假反馈）
SRV= 0 0 37 0 43 -334          ← 摇杆松开（目标0/0）后右 duty 仍钳在 -334 持续倒驱
SPD= 0 43 29926 1              ← "空闲"状态下右轮累计转出 29.9 m
```

机制：镜像减速箱电机（B/C 反向装）+ `g_encInvert` 默认全 +1（myCar `rt/encoder.c:62`）+ 0x70 判向从未跑过 → 右侧（E3+E4）聚合符号反 → 右侧伺服正反馈。

**目标**：做一个专门的标定 Web UI 页面 `/calib.html`，完成：

1. 触发 TC275 的 0x70 编码器判向自动标定；
2. 标定期间的安全互锁（禁驾驶、防连点、急停常在）；
3. 判向结果直接显示在页面上（M2）；
4. 标定后的闭环验证观察（目标 vs 实测轮速，复用现有遥测）。

**非目标（明确排除）**：

- ~~判向结果的掉电持久化~~ → **V1.1 起纳入**：标定数据（电机位置/运动方向/算法参数）存 TC275 DFlash，见 §8.3 与 myCar 34 号 §8（§7.3 的 bake-in 降级为应急路径）；
- 速度标定（myCar §15.3 的 `ENCODER_FULL_SCALE_MM_S` 整定）；
- DPT 0x71~0x79 其它产测命令（TC275 侧未实现，页面**只准发 0x70**）。

---

## 2. 页面设计（M1，纯 C6 可交付）

### 2.1 文件与入口

| 项 | 规定 |
|---|---|
| 页面 | 新增 `assets_src/calib.html` + `assets_src/calib.js`（自包含：复制控制页约 40 行的 `buildFrame/crc16/wsUrl/hello` 助手，**不重构 app.js**，降低回归面）；打包机制照 doc/12-assets-tools.md |
| 入口 | 控制页 `index.html` 的"维护 · 固件升级" `<details>` 区（73 行附近）加一行链接：`台架标定 → /calib.html` |
| 会话 | 复用同源 token 配对流程与 WS 建连（`/ws?token=`）。页面 hello 后按 `role==="ctrl"` 判定权限；服务端单 ctrl 会话会把旧 ctrl 自动降级（`c6_http/ws_sessions.c:194`），页面上提示"**标定时请勿同时操作控制页**" |

### 2.2 布局（单列卡片，沿用深色座舱主题与现有 CSS 类）

```
┌──────────────────────────────────────┐
│ ●已连接   [状态chip]      [STOP 大红钮] │  ← STOP=sendDrive(0,0)，任何状态可用
├──────────────────────────────────────┤
│ ⚠ 判向标定将逐轮转动车轮，必须四轮离地    │  ← 常显安全横幅（warn 色）
├──────────────────────────────────────┤ 卡片A 编码器判向
│ 说明：每轮 250ms 脉冲共约 1.4s；          │
│ 结果仅本次上电有效（断电回默认值）          │
│ [×] 我已确认四轮离地                    │  ← 未勾选则按钮禁用
│ [ 开始判向标定 ]   （进度条 ▓▓▓░░ 1.4s）│
│ 结果表（M2 有回传后填充）：               │
│  轮       位置      delta  判定   状态   │
│  A(motor A) 前左   -1204  →-1   已翻转  │
│  B(motor B) 后左    +331  →+1   正常    │
│  C(motor C) 后右       0  →+1   无计数⚠ │  ← delta=0 标红：查编码器接线
│  D(motor D) 前右    +987  →+1   正常    │
├──────────────────────────────────────┤ 卡片B 闭环验证观察（复用现有遥测）
│  左侧  目标 ▓▓▓▓░░ 实测 ▓▓▓▓░░          │  ← 同控制页 gauge 样式
│  右侧  目标 ▓▓▓▓░░ 实测 ▓▓▓▓░░          │
└──────────────────────────────────────┘
```

### 2.3 状态机与互锁

```
idle → confirm(二次确认弹窗) → sending(发1帧) → running(1.4s+余量,互锁) → done
                                                     ├→ 超时(3s)→ fallback 文案
                                                     └→ 收到结果(M2)→ 填表
```

互锁规则（全部硬性）：

1. "开始判向标定"仅在 `[√]四轮离地` 勾选 + WS 已连接 + `role==="ctrl"` 时可点；
2. 点击弹 `confirm()` 二次确认（文案含"车轮将逐个转动"）；
3. 一次确认只发**一帧**；`running` 窗口内（发出后 3s）按钮禁用、进度条走 1.4s；
4. `running` 窗口内本页所有 `sendDrive` 抑制、摇杆状态清零（等价 `joyEnd()`）——TC275 标定分支虽然忽略目标，但标定结束瞬间会恢复执行最新目标，不清零会突跳；
5. STOP 按钮永远可用（不受互锁限制）；
6. 防连点：上次触发后 3s 内再次点击无效。

### 2.4 超时与降级（M1 的预期终点）

发出 0x70 后 3s 内未收到结果事件 → 结果区显示固定文案：

> 结果回传未启用（需 myCar 固件 M2）：请在 TC275 调试串口查看 `ENCCAL=` 行（invert[0..3] delta[0..3]）。

页面不报错、不复位，进度照常结束。

---

## 3. 通信协议契约（M1：既有通路，零改动，逐跳验收路径）

开发 AI 按此链路逐级自验：

| 跳 | 位置 | 行为 |
|---|---|---|
| 1 | 页面 `calib.js` | `ws.send(buildFrame(0x70, new Uint8Array(0)))`；v2 帧 `[SYNC1][SYNC2][VER][0x70][seq][len=0][CRC16be]`（参考 app.js:21 `buildFrame`） |
| 2 | `c6_bridge/bridge.c:153` | `bridge_post_cmd` 入队；0x70 非 DRIVE，走严格不丢路径 |
| 3 | `c6_link/link.c:286` | v2 cmd 0x70∈[`PROTO_CMD_DPT_ENTER`,`PROTO_CMD_DPT_SELFTEST`] → SF `TYPE_CMD(0x01)`、`CID_DPT(0x04)`、`payload[0]=op=0x70`、len=1 |
| 4 | SF-over-QSPI3 | 事务泵照旧（doc/14-sf-link.md；寄存器握手 + RDDMA/WRDMA 分段） |
| 5 | myCar `com/link.c:243` | CPU2 `link_dispatch`：CID 白名单含 DPT → `link_forward(0x70,…)` 入 xcore 命令队列 |
| 6 | myCar `mw/proto/protocol.c:110` | CPU0 `PROTO_handleCommand` case 0x70 → `XCORE_dirCalibRequest()`。**注意：不是驾驶命令——不发心跳、不受故障锁存门禁** |
| 7 | myCar `rt/motor_algo.c` | CPU1 消费请求 → `calibStart`（`MOTOR_stopAll`+`SERVO_reset`）→ 逐轮 250ms +12% duty 脉冲 + 80ms 停顿，共 ≈1.4s → delta<0 翻 `ENCODER_setInvert(-1)`，delta==0 记死通道 → 结束 `ENCCAL= invert[0..3] delta[0..3]`（仅串口） |
| 8 | 任意急停 | 0x32 或链路失联 → `ENCCAL aborted (estop)`，标定中止 |

**命名冲突（必须在代码注释里写明）**：字节 0x70 在 c6_car 叫 `PROTO_CMD_DPT_ENTER`（`c6_proto/proto_frames.h:78`），在 myCar 叫 `PROTO_CMD_DPT_CAL_DIR`（`mw/proto/protocol.h:33`）——同一字节双语义，对 myCar main 基线的实际语义是**编码器判向标定**。其余 0x71~0x79 在 TC275 侧落入 `default` 被忽略，页面禁止发送。

---

## 4. 结果回传协议（M2，跨仓库契约——本文档定义，实施分两仓库任务）

现状缺口：标定结果只有 TC275 串口 `ENCCAL=` 行，页面拿不到。M2 定义如下：

### 4.1 SF 帧定义（myCar 侧）

| 字段 | 值 |
|---|---|
| TYPE | `SF_TYPE_EVT(0x05)` |
| CID | **新增** `SF_CID_DPT_RESULT = 0x22`（`mw/sf/sf_frame.h`；0x20/0x21 已占用，0x22 空闲） |
| payload | **22 B**：`[0] op u8 = 0x70`、`[1] status u8`、`[2..5] invert i8×4`、`[6..21] delta i32×4`（LE）；追加 `[22] saved u8` 后 **23 B**（≤248 上限） |
| status | 0=完成 1=急停中止 2=忙（已有标定在跑） |
| 方向 | 仅 TC275 → C6，每次标定结束发一帧 |

> **V1.1 长度更正**：本文档 V1.0 曾把 payload 标为 26 B、追加 `saved` 后 27 B——那是算术错误（`i32×4` 是 16 B 不是 20 B）。**字段偏移从未变过，线上布局以本表为准**；myCar 34 §3.1/§9.1.1/§9.4 与 doc/22 已同步改订。TC275 侧首版按 27 B 实施时踩到同一坑（`payload[22..25]` 未赋值随帧发出、C6 读到脏 `saved`），现已改为 23 B / `saved@[22]` 并在其主机单测加了长度不变式。C6 解码本就按字段偏移 + 长度下限（n≥22，n≥23 才取 saved）实现，两种长度都能收。


**myCar 侧实施路径**（另行立项，本文档只给契约）：CPU1 标定结束把结果写入新 xcore 块 `CalibResult`（照 myCar doc/33 §4 规约：lock 访问器 + `XCORE_init` 清零）；CPU0 轮询读到有效结果后 `LINK_send(SF_TYPE_EVT, 0x22, …)` 一次性发送并清标志。

### 4.2 C6 侧解码与 WS 事件

- `c6_link` 收到 `TYPE_EVT/CID 0x22` → 解码 → 经 `c6_http` JSON 事件流下发：`{"t":"cal","status":n,"invert":[i0,i1,i2,i3],"delta":[d0,d1,d2,d3]}`（与现有 `"tc"/"otastatus"` 事件同通道同风格）；
- 页面收到 `"cal"`：填 §2.2 结果表；`delta==0` 行标红"无计数：查编码器接线"；`invert==-1` 行标"已翻转"；`status!=0` 显示对应原因；
- 兼容性：无 M2 固件时（页面在 3s 窗口内收不到 `"cal"`）走 §2.4 降级文案，不得报错。

### 4.3 回归与测试

- c6_car `test/host` 的 proto 用例补 `0x70` 触发帧与 `"cal"` 事件解析用例，保持全绿；
- myCar 侧合入时同步更新 `test/host/test_sf*`（SF 层改动必跑主机单测，myCar doc/33 §7）。

---

## 5. 验收标准（AC）

**M1（本仓库独立可验收）：**

1. `/calib.html` 从控制页入口可达；未配对时能走完配对并建 WS；进入后旧 ctrl 会话被降级，两页均不崩、不重连风暴；
2. 未勾"四轮离地"时按钮禁用；一次确认只发一帧 0x70（可用浏览器 devtools WS 记录或 `/api/diag` 验证）；
3. 台架上触发后，TC275 调试串口（115200，Infineon DAS JDS COM）1.4s 内出现 `ENCCAL start` 与 `ENCCAL=`；页面进度条同窗口走完，3s 超时文案正确显示；
4. `running` 窗口内本页驾驶发送被抑制、STOP 可用；
5. 控制页回归：驾驶、车速表、里程、配对、OTA 全部不受影响。

**M2（myCar 固件合入后）：**

6. 标定完成后 1s 内结果表填充，`invert/delta` 与串口 `ENCCAL=` 行数值一致；delta=0 行标红；
7. 急停中止场景（标定中发 0x32）页面显示"中止"而非超时。

---

## 6. 任务拆分建议

> **V1.1**：本表 M3（bake-in）由 §8.5 的 M3'/M4/M5 取代；M1/M2a/M2b 不变。

| 任务 | 仓库 | 内容 | 依赖 |
|---|---|---|---|
| M1 | c6_car | calib.html/js + 控制页入口 + 互锁/进度/降级文案 | 无 |
| M2a | myCar | `SF_CID_DPT_RESULT(0x22)` 帧常量 + xcore CalibResult 块 + CPU0 发送 + 主机单测 | 无（可与 M1 并行） |
| M2b | c6_car | EVT 0x22 解码 + `"cal"` 事件 + 结果表 | M2a |
| M3 | myCar | ENCCAL 符号 bake 进 `g_encInvert` 默认值（台架确认后）+ 文档同步（23 §8.4、21 §5.2） | 台架跑 M1 |

---

## 7. 附录

### 7.1 文件锚点

- c6_car：`assets_src/index.html:62-86`（ctl 行与维护区）、`assets_src/app.js:6/21/173-205`（CMD 表/buildFrame/摇杆 30Hz）、`components/c6_bridge/bridge.c:153-184`、`components/c6_link/link.c:286-293`（DPT 映射）、`components/c6_http/ws_sessions.c:194-201`（单 ctrl 降级）
- myCar：`mw/proto/protocol.h:33`、`mw/proto/protocol.c:110-116`、`com/link.c:243`（CID 白名单）、`rt/motor_algo.c:30-57/183-288`（标定状态机）、`rt/encoder.c:62/327-339`（invert）、`mw/sf/sf_frame.h:60-96`（TYPE/CID 表）、`mw/xcore/xcore.c`（块规约样板）

### 7.2 TC275 串口行格式（台架对数用，115200 8N1）

```
SRV= tL mL dL tR mR dR     ← 1Hz，percent*10：目标/实测/输出 duty（MOTOR_ALGO_diag）
SPD= vL vR odo alive       ← 1Hz，mm/s / mm / 0|1（Cpu2_Main link_speedPrint）
ENCCAL start               ← 标定开始（要求四轮离地）
ENCCAL= i0 i1 i2 i3 d0 d1 d2 d3   ← 标定结束：各轮符号终值 + 脉冲计数 delta
ENCCAL aborted (estop)     ← 急停中止
```

### 7.3 判向结果固化（当前工作流，非本任务范围）

ENCCAL 是 RAM 态，**每次上电回到全 +1**。台架确认各轮符号后，把结果手写进 `rt/encoder.c:62` 的 `g_encInvert` 默认值，在 AURIX Development Studio（TASKING）构建烧写；同批更新 doc 23 §8.4 与 21 §5.2（myCar 红线：改行为同批改文档）。

### 7.4 给开发 AI 的红线提示

- 只发 0x70，不发 0x71~0x79；0x70 不是心跳，不要把它当驾驶保活用；
- 不改 `c6_link` 的 v2→SF 映射表（0x70 通路已存在，改了反而破坏对齐）；
- 不在标定页面引入新依赖/构建步骤；样式复用现有 class；
- 改 `index.html` 仅限加一行入口链接，其余全部放新文件。

---

## 8 · V1.1 追加需求：逐电机手动控制 · 车辆可视化 · 参数与 DFlash 持久化

**来源**：用户 2026-09-27 追加三项——①标定数据（电机位置、运动方向、算法参数）存 TC275 DFlash；②标定页可逐电机手动控制；③页面显示一辆车、体现 4 个轮子的运动状态。协议追加定义见 §8.4，TC275 侧实施契约为 **myCar doc/34 §8/§9**（同一份协议两端，数值必须逐字节一致）。页面布局在 §2.2 基础上：顶部加车辆视图，原卡片 A/B 之下新增卡片 C（逐电机控制）、卡片 D（参数与存储）。

### 8.1 卡片 C：逐电机手动控制（台架 jog）

- 4 行，按 **REC 的位置映射**显示标签（默认 A 前左 / B 后左 / C 后右 / D 前右），每行两枚**按住即转**按钮 [◀ 反转] [▶ 正转] + 当前 duty 读数；
- 交互：`pointerdown` 起以 **30 Hz** 发 `0x71 MOTOR_JOG {motor, duty=±500}`（percent*10，±50%，与固件钳位一致），`pointerup/pointerleave` 发一次 `duty=0`；固件侧另有 **300 ms 无刷新自动停**（双层保险）；
- 互锁：jog 与判向标定互斥（任一进行中另一入口禁用）；jog 期间本页 DRIVE 流抑制；STOP 永远可用（发 `duty=0` + 全部 jog 归零）；
- TC275 语义：jog 是**开环逐电机驱动**（不过伺服），受故障锁存门禁、不充当心跳、急停立即停（myCar 34 §9.3）。

### 8.2 车辆可视化（4 轮运动状态）

- **SVG 车俯视图**置于页面顶部：车身 + 4 个轮子元素（可旋转）+ 方向箭头 + 左/右速度读数；
- 数据源：复用既有 `tc` 状态事件的 `vMeasL/R`。**遥测是侧级不是轮级**（左 A/B 同显 vMeasL，右 C/D 同显 vMeasR；38 B 遥测定长布局不动，轮级遥测列为非目标）；
- 动画：轮子自转角速度 ∝ |实测速度|，方向随符号翻转；`alive=0` → 轮子置灰；急停/故障 → 车身边框红色；jog 中的轮高亮描边；
- 实现：`requestAnimationFrame` 累积角度 + CSS transform 旋转，**不引入外部库**，适配深色座舱主题。

### 8.3 卡片 D：参数与 DFlash 持久化

- 进入页面即发 `0x72 REC_GET`，用 EVT `0x23` 渲染**当前生效参数表**：每轮 位置 / 编码器方向（±1，`-1` 标"已翻转"）/ 全局 满量程速度 `fullScaleMmS` / 轮径 `wheelDiaMm` / 数据来源（默认值 · DFlash · 在线设置）；
- 编辑 `fullScaleMmS`（100..5000）、`wheelDiaMm`（30..200，前端范围校验）→ `0x73 REC_SET` **生效并写入 DFlash**；`0x74 REC_CLEAR` 恢复默认并擦除；
- `0x70` 自动判向成功后由 TC275 **自动持久化**，UI 无需手动存；EVT `0x22` 追加 `saved u8`（0 未持久化 / 1 已写 / 2 写失败），结果表显示保存状态；
- "运动方向"指**编码器计数方向 invert**（0x70 判向结果，生效项）；电机驱动方向属接线级事实（myCar `g_dirInvert` 编译期表），**不在本协议字段范围**，页面不显示、不下发。

### 8.4 协议追加（与 myCar 34 §9 同源；既有 0x70 通路与 c6_link 映射零改动）

DPT 命令族（SF `CMD` / `CID_DPT`，`payload[0]=op`；c6_link 现有 0x70~0x79 映射已覆盖，**不改 c6_link**）：

| v2 op | 名称 | payload（多字节小端） | 应答 |
|---|---|---|---|
| 0x70 | CAL_DIR（既有） | ∅ | EVT 0x22（追加 `saved u8` → 23 B，见 §4.1 更正） |
| 0x71 | MOTOR_JOG | `{motor u8, duty i16}`（3 B，duty=percent×10，钳 ±500） | 无（newest-wins，300 ms 固件超时） |
| 0x72 | REC_GET | ∅ | EVT 0x23 |
| 0x73 | REC_SET | `{pos u8×4, invert i8×4, fullScale i16, wheelDia i16}`（12 B） | EVT 0x23（生效+持久化回执） |
| 0x74 | REC_CLEAR | ∅ | EVT 0x23（默认值回执） |

> 0x71/0x73 载荷偏移已与 myCar 实码逐字节核对：jog `{motor@0, duty i16LE@1..2}`（`calib_record.c:CALIBREC_jogDecode`，钳 ±500、`motor<4` 校验）；REC_SET `{pos@0..3, invert@4..7, fullScale i16LE@8..9, wheelDia i16LE@10..11}`（`CALIBREC_recSetDecode`，`len != 12` 直接拒收，`ver` 由固件置 1、`src` 由固件置 2，**不在线上载荷里**）。

新增事件帧：**EVT `SF_CID_DPT_REC = 0x23`**，payload **15 B**，逐字节（已与 myCar `mw/calib/calib_record.c:buildEvtRec` 实码核对一致）：

| 偏移 | 字段 | 类型 |
|---|---|---|
| 0 | ver | u8 = 1 |
| 1 | src | u8：0 默认值 / 1 DFlash / 2 在线设置 |
| 2..5 | pos[4] | u8×4：0 前左 / 1 前右 / 2 后左 / 3 后右（下标 = 轮号 A..D） |
| 6..9 | invert[4] | i8×4（下标 = 轮号 A..D） |
| 10..11 | fullScaleMmS | i16 LE |
| 12..13 | wheelDiaMm | i16 LE |
| 14 | crcOk | u8 |

c6 侧改动点：`c6_link` 不改；JSON 事件流新增/扩展（`"cal"` 扩展 `saved` 字段、新 `"rec"` 事件）；`test/host` 补 0x71~0x74 触发帧与 `0x22/0x23` 解析用例。

### 8.5 里程碑与验收追加

| 任务 | 仓库 | 内容 |
|---|---|---|
| M3' | myCar | DFlash 持久化 + CalibRecord + 参数运行时化（34 §8） |
| M4 | c6+myCar | MOTOR_JOG 逐电机控制（本节 8.1 + 34 §9.3） |
| M5 | c6 | 车辆可视化 + 参数卡片（8.2/8.3） |

**AC 追加（编号接 §5）：**

9. 按住 jog 按钮对应电机即转（方向与按钮一致）、松手 ≤300 ms 停；故障锁存时 jog 被拒；急停立即停；
10. 车辆俯视图 4 轮随遥测转速/方向动画，`alive=0` 置灰，急停红色边框，jog 中轮高亮；
11. `0x70` 成功后**断电重启**：`REC_GET` 显示 `src=DFlash`、invert 与保存值一致、推杆 `SRV=` 两侧实测与 duty 同号；
12. `REC_SET` 修改 `fullScale` 后 `SRV=/SPD=` 的 pct 换算按新值生效；`REC_CLEAR` 后回到默认值且 `src=0`。
