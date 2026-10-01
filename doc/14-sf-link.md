# 14 SF 链路：SPI 从机传输层与 SF 帧（c6_sf / c6_link，22 号方案的 C6 侧落地）

| 项 | 内容 |
|---|---|
| 文档编号 | 14（esp32c6_car 本地编号，跨仓真源为 tc275_car `doc/20-design/22-link-spi-design.md`，下称"22 号"） |
| 版本 | V1.1（2026-09-27，见 §9 交互简化） |
| 代码位置 | `components/c6_sf/`（SF 帧编解码，纯 C）、`components/c6_link/`（SPI 从机传输层） |
| 状态 | 🟩 **代码完成 + G1/G2 门通过** — G1 主机单测 33/33 绿（新增 test_sf 7 项）；G2 `idf.py build` 0 error/0 warning；**G1 波形兼容（22 §8 的硬风险项）待台架** |

## 1. 范围与总原则

按 22 号改动清单 §7.1 / 跨仓待办 41 §3 T1–T5 执行：

- 传输层 UART → **`spi_slave_hd` 段模式从机**（TC275 QSPI3 主机 ⇄ C6 SPI2 从机 + IRQ 握手线）；
- LINK 段帧格式 → **SF 帧**（22 §5），手机 WS 侧保持 proto v2 不变；
- **`link.h` 对外 API 保持帧级 v2 语义**：`link_send(proto_frame_t)` 签名不变，v2↔SF 字段映射在 c6_link 内部完成（22 §7.1 原写"在 c6_bridge 里做"，落地改为 link 内做——bridge/pair/ota_relay 完全不感知链路层，star 解耦更彻底；差异已记录待 22 号升版时回写）；
- 删除全部 UART 专有物：0x44 波特率协商、PING/PONG、`C6_LINK_TX/RX_GPIO`（T2/T3 达成，全仓 grep 无残留）。

## 2. 接线与配置（与 23-wiring §9.1 一一对应）

| 信号 | TC275 | C6 GPIO | Kconfig |
|---|---|---|---|
| SCLK | X1-3 P33.11 | 19 | `C6_LINK_SPI_SCLK_GPIO` |
| MOSI | X1-4 P33.12 | 18 | `C6_LINK_SPI_MOSI_GPIO` |
| MISO | X1-5 P33.13 | 20 | `C6_LINK_SPI_MISO_GPIO` |
| CS | X1-12 P23.4 | 23 | `C6_LINK_SPI_CS_GPIO` |
| IRQ（开漏，数据就绪） | X1-8 P23.0 | 21 | `C6_LINK_SPI_IRQ_GPIO`（10k 上拉为外部件） |

- 时钟档位：`C6_LINK_SPI_CLOCK_HZ`（默认 5 MHz 量产基线；主机驱动时钟，从机仅镜像用于诊断）；
- 调试串口 GPIO6/7（23 §2）：Kconfig 中以"文档锚点"项固化（T3 三方一致），LINK 驱动不占用。

## 3. 共享寄存器（22 §4.3，C6 从机发布；主机**单次连读 + 钳位容错**，见 §9）

| 偏移 | 名称 | C6 侧实现 |
|---|---|---|
| 0 | `SF_READY` | 初始化末尾写 `0x5F534601`；之前为 0，主机不得发数据事务 |
| 4 | `SF_TX_PENDING` | 当前在途 TX 段字节数（单段在途模型，主机读走后清 0） |
| 8 | `SF_RX_ROOM` | 已排队 RX DMA 缓冲可写字节（2×512B 轮换） |
| 12 | `SF_ALIVE` | 10 ms esp_timer 递增（掉电即停 → 主机 500 ms 判失联） |
| 16 | `SF_ERRSTAT` | 位图：CRC/FMT/SEQ/RXOVFL/TXOVFL/TRUN/LINKLOST（置位粘滞） |
| 20 | `SF_CMDRSP` | GEN 回执 `{u8 cmd, u8 result, u16 rsv}` |
| 24 | `SF_GEN`（主机→从机） | 主机 WRBUF 写 4 B 命令槽：NOP/RESET_LINK/SILENCE_ON/OFF/CLOCK_SET |

## 4. 事务与任务模型

```
link_task (prio 12, 5 KB, TWDT 订阅):
  初始化: 排队 2 个 RX 段缓冲 → SF_READY=1
  循环 (事件驱动, 100 ms 兜底 tick):
    ├─ ISR 通知位: tx_done / rx_done / host_event / gen
    ├─ host_event → 活性刷新 (任何主机事务都喂看门狗)
    ├─ gen → 读 SF_REG_GEN 执行 GEN 命令 → 回执 SF_CMDRSP
    ├─ tx_done → TX_PENDING=0 → IRQ 拉低
    ├─ rx_done → SF 解析段 → v2 帧 → q_rx → 重排 RX 缓冲 → RX_ROOM 更新
    ├─ link_tx_kick: SF 帧队列 → 装配 ≤512B 段(多帧+4B 补零) → queue_trans
    │              → TX_PENDING → IRQ 拉高
    └─ 健康看门狗: 500 ms 无主机事务 → LINK DOWN (22 §5.4)

ISR 回调 (cb_sent/cb_recv/cb_buffer_tx/cb_buffer_rx): 仅置通知位 + 唤醒任务
alive esp_timer (10 ms): SF_ALIVE++ → write_buffer（ISR-safe，官方例程同款）
```

- **命令下行时延**：C6 收到手机命令 → SF 帧入队 → IRQ 拉高 → TC275 IOM 中断即时开事务；IRQ 丢失由 TC275 2 ms 保活轮询兜底（22 §4.1）；
- **吞吐**：5 MHz 下单段 512 B ≈ 210 µs，OTA 1 MB ≈ 2 s 传输（T5 判据 ≤3 s）。

## 5. SF 帧（编解码落在 `c6_sf`，主机单测覆盖）

```
| 5A | 01 | TYPE | SEQ | FLAGS | LEN u16LE(≤248) | CID | DATA[LEN] | CRC16 |
CRC = CRC16-CCITT-FALSE（复用 c6_proto 同一实现），前 8+LEN 字节；
段内多帧串接，段尾 0x00 补齐到 4 的倍数（解析器 idle 态跳过 0x00）；
SEQ 严格前进窗口 1..(mod 256)..32，每方向独立（22 §5.3）；
**连续 8 帧越窗（`SF_SEQ_RELOCK_RUN` / `LINK_SEQ_RELOCK_RUN`）即丢弃窗口、下一帧重新锁定**——任一板复位后 ~0.3 s 内自愈，替代原 GEN RESET_LINK 重同步握手（22 §5.3 升版回写项）。
```

TYPE/CID 全表见 `sf_frame.h`（与 22 §5.2 一致）。落地补充（待 22 号升版回写）：

| 补充项 | 决策 |
|---|---|
| OTA ABORT | 22 §5.2 的 OTA CID 只列 0x30–0x34；C6 新增 `SF_CID_OTA_ABORT=0x35`（OTA_D 方向）承载 v2 0x65 |
| 0x42 LINK_STATE | SF 侧映射为 `CMD/CID_DIAG`，payload `{u8 op=0x42, u8 state}`（C6→TC275 的客户端数通知，FR-5 语义不变） |
| v2 方向档 0x02–0x09/0x20/0x21/0x30–0x32 | 与 0x50 DRIVE 同走 `CMD/CID_DRV`，payload `{u8 op=v2cmd, i16 v, i16 w}`（"子码区分"的具体化；v2 载荷信息零丢失） |
| 0x53 DIAG | `CMD/CID_DIAG`，payload `{u8 op=0x53, 原载荷}` |
| SF ACK/EVT 到手机侧 | 通用 ACK 与 EVT 错误 → v2 0x53 隧道帧进 bridge，桥转 WS 文本 `{"t":"evt",...}` |
| PAIR 回复（TC275→C6） | `EVT/CID_EVT_STATE(0x21)`，payload `{u8 kind=1, v2 0x51 数据}` → 还原 v2 0x51 帧交 pair |

## 6. 上下文与安全要点

- `link_send/link_send_ota_chunk`：互斥（10 ms 超时）+ 帧队列（32 深），满返回 `ESP_ERR_NO_MEM`（BUSY），命令类反压不丢；
- RX 解析器错误分支：CRC 错连续 5 帧 → `LINK DOWN` 事件 + `SF_ERRSTAT.LINKLOST`（22 §5.4）；
- DMA 缓冲静态分配（2×512 RX + 1×512 TX），无堆驻留；
- OTA 写 flash 期间的链路抖动（22 R10）：V1.0 未加保活插桩，台架实测后决定（记录于 13 号文档）。

## 7. 验证状态

| 门 | 内容 | 状态 |
|---|---|---|
| G1 主机单测 | test_sf 7 项：回环/段多帧补零/错误分支/SEQ 窗口/10⁷ 模糊/映射回环/CHUNK 242B 布局 | ✅ 全绿（全套 33/33） |
| G2 编译 | `idf.py build` 0 error / 自研组件 0 warning | ✅ |
| 22 §8 G1 波形兼容 | TC275 数据字节模拟 CMD/ADDR/DUMMY 能否被 HD 从机解析 | 🔴 待台架（唯一硬风险，22 R7） |
| 22 §8 G3–G6 | 时延/安全语义/提速阶梯/老化 | 🔴 待台架 |

## 8. 与 22 号方案的差异记录（回写建议）

| # | 差异 | 理由 |
|---|---|---|
| D-1 | v2↔SF 映射从 c6_bridge 移到 c6_link 内部 | `link.h` 保持 v2 帧 API（22 §7.1 自身要求"bridge 不感知"），映射是链路容器转换，属于链路层职责；bridge 仅新增 EVT→WS 文本一行 |
| D-2 | TX 采用"单段在途"而非多段流水 | TX_PENDING 语义简单无歧义（段长即待读量），5 MHz 下吞吐足够；G5 提速后若拥塞再改多段环形 |
| D-3 | OTA CID 0x35 ABORT、CID_DIAG op=0x42 | 22 §5.2 未覆盖的 v2 语义载体（见 §5 表） |

## 9. 交互简化（2026-09-27，双端联动；台架"无法连接"类故障的根因修复）

实测（负载下随机断连、复位后长时间无法恢复）定位出四个机制性问题，按下表简化，**C6 侧已落地，tc275_car 侧（`com/link.c`、`mw/sf/`）同步修改待编译烧录**：

| # | 原机制 | 问题 | 简化后 |
|---|---|---|---|
| S-1 | 主机每周期**连读两次寄存器、比对 4 个决策字段**，3 次不一致 → regUnstable → LINK_LOST + 急停 | 从机段完成/10ms ALIVE 都会即时改写寄存器，链路越忙比对越容易失败 → **负载越重越容易误判失联急停**（"空载正常、满载断连"的根因） | 单次连读；READY 魔数不对 → 静默 LINK_DOWN（下一周期好读即恢复）；TX_PENDING ≤640、RX_ROOM ≤1024 钳位；**LOST 只由 ALIVE 500ms 停走判定** |
| S-2 | 主机 LINK_READY 沿 → GEN RESET_LINK 握手（NOP+命令，各等 20ms 回执）后才恢复收发；失败则下周期重试 | 从机不回执时**活锁**：状态 READY 但永不行数据事务，遥测队列静默溢出（"复位后无法连接"的根因之一） | 整个握手从泵中移除（`LINK_gen` 保留作台架/诊断入口）；READY 沿主机仅 `SF_parserInit`，SEQ 窗口靠 S-4 自愈 |
| S-3 | SEQ 越窗帧一律丢弃、窗口永不重锁（两端口径相同） | 任一板复位后计数器落窗外，接收端**最多拒收 224 帧（30Hz 驱动 ≈ 7.5s 控制失灵）**直到 u8 计数绕回 | 两端解析器一致：连续 `8` 帧越窗 → 丢窗重锁，最坏拒收 8 帧（≈0.27s） |
| S-4 | C6 TX 段在途后完全依赖主机 RDDMA 取走；`link_rx_requeue` 失败即永久少一个缓冲 | 主机中途消失/完成事件丢失 → TX_PENDING 永挂起、TX 路饿死；RX 缓冲丢失 → RX_ROOM 永久缩水 | TX 在途 **2s 未被读走自动重排**（`LINK_TX_STALL_MS`）；RX 重排失败标记后每循环重试 |

`C6_BENCH_CTRL` 台架联调不受影响；GEN 命令集（SILENCE/CLOCK_SET/RESET_LINK）语义不变，RESET_LINK 降级为诊断入口。
