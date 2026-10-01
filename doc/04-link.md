# 04 LINK 链路（c6_link）— SPI 从机传输层

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_link/link.c`、`include/link.h`、`Kconfig`；SF 帧编解码见 `components/c6_sf/` 与 [14-sf-link.md](14-sf-link.md) |
| 上游需求 | tc275_car `doc/20-design/22-link-spi-design.md`（V1.0 选定方案）、SDD V1.2 §3.7/§6.1a、FR-3/FR-5 |
| 状态 | 🟩 **代码完成，G1/G2 通过** — 波形兼容（22 §8 G1）与时延/安全门禁（G3–G6）待台架 |

> **V1.0 变更**：物理层已由 UART@2M 换向 SPI 半双工从机（22 号方案，T1 落地）。
> 本篇重写于 2026-09-26；UART 版描述（波特率握手/PING 心跳/0x44）已随代码删除，
> 历史 口径见 git 历史。

## 1. 职责

TC275 QSPI3 主机 ⇄ C6 SPI2 从机（`spi_slave_hd` 段模式）之间的帧链路：
SPI 事务与 DMA 缓冲管理、共享寄存器握手（SF_READY/TX_PENDING/RX_ROOM/ALIVE/
ERRSTAT/CMDRSP）、IRQ 数据就绪线驱动、SF 帧↔v2 帧字段映射、链路健康监测
（500 ms 主机静默 → DOWN）。

## 2. 上下文与任务模型

| 任务/上下文 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| `link_task` | 12 | 5 KB | RX 段解析 → v2 帧 → `q_rx`（16 深）；TX 段装配（≤512B 多帧+4B 补零）→ TX_PENDING/IRQ；寄存器刷新；500 ms 主机看门狗；GEN 命令处理；TWDT 已订阅 |
| ISR 回调 ×4 | — | — | cb_sent/cb_recv/cb_buffer_tx/cb_buffer_rx：置通知位 + 唤醒任务（主机任何事务 = 活性喂狗） |
| `alive` esp_timer | 10 ms | — | SF_ALIVE++ 写共享寄存器（掉电即停 → 主机判失联） |

线程安全：`link_send/link_send_ota_chunk` 互斥（10 ms 超时）+ 队列背压（满 = BUSY，
命令类不静默丢——LLDD 满策略保留）；bridge/pair 仍只调 `link_send()`。

## 3. 硬件配置（真源：tc275_car 23-wiring §9.1）

| 信号 | TC275 | C6 GPIO | 说明 |
|---|---|---|---|
| SCLK | P33.11 (X1-3) | 19 | 主机驱动时钟 |
| MOSI | P33.12 (X1-4) | 18 | 主→从数据 |
| MISO | P33.13 (X1-5) | 20 | 从→主数据 |
| CS | P23.4 SLSO5 (X1-12) | 23 | |
| IRQ | P23.0 (X1-8) | 21 | **开漏输出**，数据就绪拉高；10 kΩ 上拉为外部件（22 §3.2） |

时钟阶梯：1 → 2 → **5（量产基线）** → 10 → 20 MHz（22 §8 G5 逐档门禁）。
`command_bits/address_bits/dummy_bits = 8`（22 §4.4/E3）；前导相位能否被 TC275
数据字节正确模拟 = 22 §8 G1 台架门禁（方案唯一硬风险 R7，两条退路已备案）。

## 4. 健康监测（22 §5.4，替代 UART 时代的 PING/静默）

```
主机活性: 任一 SPI 事务（读寄存器/WRDMA/RDDMA/GEN）刷新看门狗
  └ 500 ms 无事务 → LINK DOWN 事件（→ bridge 通知页面 + OTA 中继 ABORT）
CRC 突发: 连续 5 帧 CRC 错 → LINK DOWN + SF_ERRSTAT.LINKLOST
SEQ 违例: 窗口外帧丢弃 + ERRSTAT.SEQ（不锁存，区别于安全故障）
RTT: 主机轮询间隔滑动估计（telemetry linkRtt 展示用；精确 RTT 由主机测）
```

## 5. 接口（link.h 全量；v2 帧 API 与 LLDD §3.3 兼容）

```c
esp_err_t   link_init(void);
esp_err_t   link_send(const proto_frame_t *f);      /* v2→SF 映射后入队, 满=NO_MEM */
esp_err_t   link_send_ota_chunk(uint16_t idx, const uint8_t *data, size_t n);
                                                     /* SF OTA_D/0x31, n≤240 (T5)  */
QueueHandle_t link_rx_queue(void);                   /* v2 帧 → bridge QueueSet    */
QueueHandle_t link_event_queue(void);                /* LINK_EV_UP / LINK_EV_DOWN  */
void        link_get_health(link_health_t *out);     /* state/clock_hz/rtt/err 计数 */
bool        link_is_up(void);
void        link_set_tap(void (*tap)(const proto_frame_t *f));  /* v2 帧镜像      */
```

**已删除**（T2）：`link_request_baud()`、`LINK_EV_BAUD_CHANGED`、0x43 PING、
0x44 BAUD、UART 引脚 Kconfig。v2 协议头中 0x43/0x44 常量同步移除（WS 侧本就不使用）。

`link_health_t`：`baud` → `clock_hz`；`/api/diag` JSON 键 `baud` → `clock`。

## 6. 资源

| 项 | 值 |
|---|---|
| DMA 缓冲 | RX 2×512 B + TX 1×512 B（静态，无堆驻留） |
| 队列 | q_tx 32×260 B + q_rx 16×80 B ≈ 9.6 KB |
| 任务栈 | 5 KB（TWDT 5 s 订阅） |

## 7. 验证状态与完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| L-1 | spi_slave_hd 初始化（段模式，8/8/8 前导，DMA 自动通道） | ✅ | |
| L-2 | 共享寄存器 6+1 映射与刷新 | ✅ | GEN 回执/静默模式/时钟镜像已实现 |
| L-3 | RX 段解析 → v2 帧 → bridge | ✅ | SF 单测覆盖解析器；板级待台架 |
| L-4 | TX 段装配（多帧+补零）+ IRQ | ✅ | |
| L-5 | v2↔SF 映射（22 §5.5 + 落地补充） | ✅ | test_sf 映射回环；全表见 14 号文档 §5 |
| L-6 | 健康看门狗（500 ms/CRC 5 连错） | 🟩 | 逻辑完成，时序指标待台架 |
| L-7 | OTA CHUNK 240 B 快路径 | ✅ | test_sf 布局用例 |
| L-8 | TWDT 订阅（01 文档缺口 S-2 修复） | ✅ | link_task 已 add/reset |
| L-9 | 22 §8 G1 波形兼容 | 🔴 | 需 TC275 侧最小验证代码 + 台架 |
| L-10 | G3 时延 / G4 安全语义 / G5 提速 / G6 老化 | 🔴 | 待台架（13 号文档清单） |
