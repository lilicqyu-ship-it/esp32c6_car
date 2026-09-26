# 04 LINK 帧链路（c6_link）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_link/link.c`、`include/link.h`、`Kconfig` |
| 上游需求 | LLDD §4.5（链路设计）、§3.1（使用纪律）、FR-3/FR-5 |
| 状态 | 🟡 **90%** — 功能全量实现；TWDT 订阅缺口 + HIL 待测 |

## 1. 职责

TC275 之间的 2 Mbps 全双工帧链路：UART1 驱动封装、帧装配（调用共享 c6_proto）、
链路健康监测（PING/静默看门狗/RTT 测量）、波特率握手（921600↔2M 两档）、
TX 帧队列（单一出线编排）。

## 2. 上下文与任务模型

| 任务/上下文 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| `link_rx` 任务 | 12 | 4 KB | `uart_read_bytes` → `proto_parser_feed` → 分帧入 `q_rx`（16 深）；PONG/BAUD 帧内部消化；tap 镜像回调 |
| `link_tx` 任务 | 11 | 3 KB | 排空 `q_tx`（8 深）→ `uart_write_bytes` |
| `link_health` esp_timer | 系统 | — | 20 ms 节拍：静默看门狗、PING（每 5 拍=100 ms）、降速/升速判定 |

线程安全：`link_send()` 内部互斥（10 ms 超时）+ FreeRTOS 队列天然线程安全；
bridge_task、pair（httpd 上下文经注入输出）、health timer 三方都只调 `link_send()`。

## 3. UART 配置

| 项 | 值 | 来源 |
|---|---|---|
| 控制器 | UART1 | 固定 |
| 引脚 | TX=GPIO10 / RX=GPIO11 | `CONFIG_C6_LINK_TX/RX_GPIO`（LLDD Q1，EE 评审后改配置） |
| RX 环 / TX 环 | 4 KB / 2 KB | LLDD §4.5 |
| 起始波特率 | 921600 | 常量 `LINK_BAUD_BASE` |

## 4. 健康监测状态机（20 ms 节拍）

```
每拍:
  ticks++
  ┌─ 静默看门狗: now-last_rx > 500ms 且当前 UP
  │    → state=DOWN、baud 回 921600、发 LINK_EV_DOWN
  ├─ 每 5 拍: 发 PING(type=PING)，记 last_ping_ms
  ├─ 误码窗口: crc_err_window ≥ 10 且当前 2M → 发 BAUD_REQ(921600)
  └─ 升速: 当前 921600 且 UP 且窗口 0 且 ticks ≥ 30s/20ms 且无在途请求
       → 发 BAUD_REQ(2M)

RX 帧事件:
  任何帧 → last_rx 刷新；首次 → LINK_EV_UP
  PONG   → rtt = now - last_ping_ms
  BAUD   → ACK(在途请求匹配) → set_baudrate + LINK_EV_BAUD_CHANGED
           REQ(TC275 发起)   → 应用并回 ACK
           NAK               → 取消在途请求
```

## 5. 接口（link.h 全量，与 LLDD §3.3 签名一致）

```c
esp_err_t   link_init(void);
esp_err_t   link_send(const proto_frame_t *f);       /* 满=ESP_ERR_NO_MEM(BUSY) */
QueueHandle_t link_rx_queue(void);                     /* bridge QueueSet 成员     */
QueueHandle_t link_event_queue(void);                  /* UP/DOWN/BAUD_CHANGED     */
void        link_get_health(link_health_t *out);       /* 读即清 crc/fmt 计数      */
esp_err_t   link_request_baud(uint32_t baud);
bool        link_is_up(void);
void        link_set_tap(void (*tap)(const proto_frame_t *f));  /* legacy 桥镜像 */
```

```c
typedef struct { link_state_t state; uint32_t baud, rtt_ms, frames_rx, frames_tx, tx_busy, last_rx_ms;
                 uint16_t crc_errs, fmt_errs; } link_health_t;
typedef struct { link_event_id_t id; uint32_t baud; } link_event_t;
```

## 6. TX 满策略（LLDD §4.5）

- `link_send` 队列满 → 返回 `ESP_ERR_NO_MEM`；bridge 命令泵收到即回 WS 错误
  JSON（不静默丢）；遥测类由 TC275 下一周期自然覆盖。
- `link_send` 实际写满由 `q_tx`（8 × 74 B ≈ 0.6 KB）背压，物理出线 2 Mbps。

## 7. 资源

| 项 | 值 |
|---|---|
| RX/TX 队列 | 16×80 B + 8×76 B ≈ 1.9 KB |
| UART 环 | 4 KB RX + 2 KB TX（驱动内） |
| 任务栈 | 4 KB + 3 KB |

## 8. 验证状态

- 编译级验证（G2）通过；解析正确性由 c6_proto 主机单测背书。
- **HIL 待测项**：30 s 升速握手全流程、误码注入降速、500 ms 静默判 DOWN 时序（≤10 ms 发 LINK_STATE）、拔线恢复。

## 9. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| L-1 | UART1 收发 + 分帧 | ✅ | |
| L-2 | 健康监测（PING/静默/RTT） | 🟩 | 逻辑完成，时序指标待 HIL |
| L-3 | 波特率握手（双向） | 🟩 | 同上 |
| L-4 | TX 队列 BUSY 语义 | ✅ | |
| L-5 | tap 镜像（legacy 桥） | ✅ | |
| L-6 | link_rx/link_tx 任务 TWDT 订阅 | 🔴 | 仅 bridge_task 订阅；LLDD §2.3 要求 uart_evt_task ✔——待补 `esp_task_wdt_add` + 循环内喂狗 |
| L-7 | 事件队列满丢弃计数 | 🟡 | `q_evt`(8) 满时静默丢；未计数（对 LINK_STATE 原子语义影响低，因事件由边沿触发重发） |
