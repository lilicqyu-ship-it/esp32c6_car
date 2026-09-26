# 08 三台泵（c6_bridge）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_bridge/bridge.c`、`include/bridge.h` |
| 上游需求 | LLDD §4.6（核心组件）、§2.3/§2.4（任务与数据流）、FR-2/FR-5/FR-6；链路段容器已换 SF 帧（[14-sf-link.md](14-sf-link.md)），bridge 对此无感知 |
| 状态 | 🟩 **97%** — 信用窗时序待 HIL（22 §8）；首屏状态缓存未入 HELLO |

## 1. 职责

系统唯一的"WS 广播 + LINK 出线编排"上下文：命令泵、遥测广播器、OTA 中继泵
三台状态机全部运行在 `bridge_task`（prio 10 / 6 KB / TWDT 5 s 已订阅）。

## 2. 上下文与队列（LLDD §2.4 映射）

```
命令下行: WS(httpd 任务) ──q_cmd(32, 满则该 WS 报错)──┐
遥测上行: link_rx 任务 ──q_link_rx(16)────────────────┤
链路事件: link 事件队列(8) ───────────────────────────┤ QueueSet
20ms 节拍: esp_timer → task notification ─────────────┘
                    ▼
              bridge_task（单上下文）
                    ├──► link_send()（TX 队列）
                    └──► ws_broadcast_binary / http_broadcast_ctl
```

时延预算：WS 收包 → LINK TX 出线 ≤1 ms（回调只做校验+入队，bridge 收到即
`link_send` 无阻塞调用）；LINK RX → WS 广播出线 ≤1 ms（邮箱单槽覆盖）。

## 3. 命令泵（§4.6.1）

`q_cmd` 逐帧 → `link_send()` → BUSY 时向源 WS 回 `{"t":"err","e":"link_busy"}`。
单帧处理无阻塞调用；role/SEQ 已在 httpd 侧完成（两道闸门中的第一道）。

## 4. 遥测广播器（§4.6.2）

- 0x41 帧 → `proto_telemetry_decode` → **单槽邮箱**（新覆旧，互斥 <5 µs）+
  `state/fault_code` 抽出维护页面状态缓存；
- 20 ms 节拍（task notification）→ 邮箱快照 → 重组 0x41 帧 →
  `ws_broadcast_binary`（逐客户端 pacing，见 07 文档 §5）。

## 5. LINK_STATE（0x42，仅 C6 发出，FR-5）

触发源（全部事件驱动，毫秒级）：
- WS 会话增减（`http_on_session_change` → `bridge_notify_clients`）；
- AP 站点增减（`net_on_ap_clients` → 同上）；
- LINK UP/DOWN/波特率变化（link 事件）；
- 配对状态变化（PAIR 帧路由后）。

聚合规则：`ws_controller_sd()≥0 → 2`，否则 `ws_client_count()>0 → 1`，否则 0；
与上次发送值相同则抑制（边沿触发），LINK 状态翻转强制重发。

## 6. OTA 中继泵（§4.6.3，信用窗口流控）

```
POST /ota/tc275(httpd)
  │ begin: LINK UP? → 初始化(窗口=8 信用, chunk=240B) → 0x60 BEGIN{total, crc=0*}
  │ feed:  按 ≤240B 子块切分（SF OTA_D/0x31 载荷上限，22 §5.5 T5），每子块取信用一次
  │        (阻塞≤2s, 无信用=窗口耗尽→暂停读HTTP) → 组 0x61 CHUNK{idx,data}
  │        → link_send；副本存 in-flight 环(8 槽)供重发
  │ finish: 等 relay_acked == chunk_count（≤30s，期间 STATUS/SWAP 照常透传）
  ▼
bridge_task:
  0x62 ACK{idx,result} → relay_acked 前推 + 信用+1 + in-flight 出环 + resend 清零
                         全部 ACK → 0x63 STATUS{DONE,100}
  0x63 STATUS{state,pct} → 文本广播 otastatus（进度条）
  0x53 DIAG 隧道（SF EVT/ACK，22 §5.5）→ 文本广播 evt
  0x64 SWAP_REQ          → 文本广播 otaswap + 泵复位（TC275 随即复位切槽）
  0x65/超时: ACK 静默 2s → in-flight 头部块重发 1 次；再超时 → 0x65 ABORT
             → 广播 otaerror + 泵复位
  LINK DOWN / 手机断开(sink.abort) → 立即 0x65 ABORT + 复位
```

\* BEGIN 的 crc32 字段保留（整图 CRC 由 TC275 收全后自行计算，SDD §9.2）。

堆安全：上传流不进 bridge 堆——块数据经 httpd 栈 → in-flight 静态环（8×66 B≈0.5 KB），
信用窗天然限制在途量（8×240 B），512 KB 堆不被吃穿（LLDD §4.6.3 设计动机）。
CHUNK 经 `link_send_ota_chunk()` 走 SF 快路径出线，bridge 不感知 SF 容器。

## 7. 接口（bridge.h 全量）

```c
esp_err_t bridge_start(void);
esp_err_t bridge_post_cmd(const proto_frame_t *f, int sd);  /* WS 命令入口 */
void      bridge_notify_clients(void);                       /* → LINK_STATE */
void      bridge_notify_pair(void);
void      bridge_send_frame(const proto_frame_t *f);         /* pair 注入用  */
const proto_telemetry_t *bridge_telemetry_snapshot(void);    /* 首屏/诊断    */
bool      bridge_link_up(void);
esp_err_t ota_relay_begin(int sd, size_t total);
esp_err_t ota_relay_feed(int sd, const uint8_t *chunk, size_t n);
esp_err_t ota_relay_finish(int sd, char *json, size_t cap);
void      ota_relay_abort(int sd);
```

## 8. 资源

| 项 | 值 |
|---|---|
| bridge_task | prio 10 / 6 KB / TWDT 5 s |
| q_cmd | 32 × 84 B ≈ 2.7 KB |
| in-flight 环 | 8 × 518 B ≈ 4.1 KB（静态） |
| 邮箱 | 38 B + 互斥 |

## 9. 验证状态

编译级验证（G2）。信用窗推进/重发/断电恢复为 LLDD §9 HIL 用例，未执行。

## 10. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| B-1 | 命令泵（≤1 ms 路径 + BUSY 回错） | ✅ | |
| B-2 | 遥测邮箱 + 20 ms pacing 广播 | ✅ | |
| B-3 | LINK_STATE 事件驱动聚合 | ✅ | |
| B-4 | 中继信用窗口（8×240B chunk，SF 出线） | 🟩 | 待 HIL 实测推进时序（22 §8 G5 含 OTA 全流程） |
| B-5 | ACK 超时重发→ABORT | 🟩 | 逻辑完成，断电注入待测 |
| B-6 | 手机断开立即 ABORT | 🟩 | 本轮修复（sink.abort 经 http close/recv 错误触发） |
| B-7 | state/faultCode 首屏缓存进 HELLO | 🔴 | 缓存已维护（car_state/fault_code），HELLO 未携带——页面靠 20 ms 遥测首帧兜底 |
| B-8 | 中继进度写遥测扩展字段 | ⚪ | LLDD 提及"写入遥测扩展字段"；0x41 帧无扩展位，进度走 WS 文本面（页眉进度条） |
