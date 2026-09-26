# 02 proto v2 帧编解码（c6_proto）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_proto/proto_frames.h/.c`（单一实现，C1 决策） |
| 上游需求 | SDD V1.2 §6.1b（手机 WS 段 = v2 帧）、原 LLDD §3.1 |
| 状态 | ✅ **100%** — 主机单测 12 项全绿（含 10⁷ 随机帧模糊） |

> **V1.0 口径更新（2026-09-26）**：v2 帧现在**只用于手机/WS 一侧**；板间 LINK
> 已换向 SPI 并改用 SF 帧（SDD V1.2 §6.1a / myCar doc 22），v2↔SF 字段映射在
> c6_link 内完成（见 [14-sf-link.md](14-sf-link.md) §5）。0x43 PING / 0x44 BAUD
> 为 UART 时代产物，常量已从本组件删除（22 号 T2）。

## 1. 职责

LINK（C6↔TC275）与 WS 二进制通道共用的唯一帧编解码实现：
帧序列化、逐字节解析状态机、CRC16、遥测载荷编解码、LE 字段访问帮助函数。
**硬规则**：纯 C99、无任何 OS/IDF/TriCore 头、无动态内存——同一文件被
C6（RISC-V 小端）、TC275（TriCore 大端，待升级 v2 时直拷）、主机单测三方编译。

## 2. 帧格式

```
| AA | 55 | VER=0x02 | CMD | SEQ | LEN | DATA[LEN] | CRC16 |
  同步字    版本      命令  序号  长度  ≤64 字节    CRC16-CCITT-FALSE

CRC16 规格（决策 C5）：poly 0x1021，init 0xFFFF，MSB first，无反射，
无异或输出；check("123456789") == 0x29B1（单测断言）。
```

帧长上限：`PROTO_HEADER_LEN(6) + 64 + 2 = 72` 字节（`PROTO_MAX_FRAME`）。

## 3. 命令表（proto_frames.h 全量常量）

| 段 | CMD | 名称 | 载荷 | 方向 |
|---|---|---|---|---|
| 遗留 | 0x01–0x09,0x10,0x20,0x21,0x30,0x31,0x32 | STOP/FORWARD/…/EMERGENCY_STOP | demo 兼容，语义归 TC275 | 手机→车 |
| 遥测 | 0x41 | TELEMETRY | 38 B 定长（见 §4） | 车→C6→手机 |
| 链路 | 0x42 | LINK_STATE | `{u8 state}` 0 无客户端/1 仅观赛/2 控制端 | **仅 C6→TC275** |
| 链路 | 0x43 | PING/PONG | `{u8 type}` PONG 回显 SEQ | 双向，C6 100ms 发起 |
| 链路 | 0x44 | BAUD | `{u32 baud, u8 op(REQ/ACK/NAK)}` | 双向握手 |
| 控制 | 0x50 | DRIVE | `{i16 v, i16 ω}`（兼心跳） | 手机→车 |
| 控制 | 0x51 | PAIR | `{op(REQ/CONFIRM/NOTIFY/REJECT), …}` | 三方 |
| 控制 | 0x52/0x53 | CFG / DIAG | 透传 | 手机↔车 |
| OTA | 0x60–0x65 | BEGIN/CHUNK/ACK/STATUS/SWAP/ABORT | 见 09/08 文档 | C6↔TC275 |
| 产测 | 0x70–0x79 | DPT 帧组 | 治具令牌后透传 | 治具↔车 |

决策 C4：LLDD 中 0x63 同时被写为 END 与 STATUS——统一为 **OTA_STATUS**，
`state==DONE` 承载 END 语义。

## 4. 遥测载荷（0x41，38 B，显式小端）

| 偏移 | 字段 | 类型 | 偏移 | 字段 | 类型 |
|---|---|---|---|---|---|
| 0 | seq | u32 | 19 | battery_mv | u16 |
| 4 | uptime_ms | u32 | 21 | battery_pct | u8 |
| 8 | state | u8 | 22 | odo_session_mm | u32 |
| 9 | fault_code | u16 | 26 | odo_total_mm | u32 |
| 11 | v_target_l/r | i16×2 | 30 | link_rtt_ms | u16 |
| 15 | v_meas_l/r | i16×2 | 32 | link_err_rate | u8 |
| 33 | fw_ver | u32 | 37 | hw_rev | u8 |

`proto_telemetry_encode/decode` 做逐字段字节搬运，**不做结构体指针强转**。

## 5. 解析状态机

```
proto_parser_feed(p, byte, out) → PROTO_RX_NONE / FRAME / CRC_ERR / FMT_ERR / VER_ERR

状态流: SYNC1 → SYNC2 → VER → CMD → SEQ → LEN → DATA[LEN] → CRC_HI → CRC_LO
  - SYNC2 收到非 0x55：若又是 0xAA 则保持再同步（AA AA 55 容错）
  - VER ≠ 0x02       → VER_ERR 并回到 SYNC1
  - LEN > 64         → FMT_ERR 并回到 SYNC1
  - CRC 不符         → CRC_ERR 并回到 SYNC1（畸形帧计数进 link_health）
```

任何字节序列输入都不会越界或卡死（模糊测试证明，见 §8）。

## 6. 接口（全量）

```c
uint16_t proto_crc16(const uint8_t *data, size_t len);
size_t   proto_encode(const proto_frame_t *f, uint8_t *out, size_t cap);   /* 0=错 */
size_t   proto_build(uint8_t cmd, uint8_t seq, const uint8_t *data, size_t len,
                     uint8_t *out, size_t cap);
void     proto_parser_init(proto_parser_t *p);
proto_rx_ev_t proto_parser_feed(proto_parser_t *p, uint8_t byte, proto_frame_t *out);
size_t   proto_telemetry_encode(const proto_telemetry_t *t, uint8_t *d, size_t cap);
int      proto_telemetry_decode(const uint8_t *d, size_t len, proto_telemetry_t *t);
/* 内联: proto_put_u16/u32, proto_get_u16/u32（LE 访问） */
```

## 7. 资源

`proto_parser_t` = 1+4×72+2+2 ≈ 90 B（栈上即可）；无堆、无锁（调用方持有）。

## 8. 验证状态（test/host/test_proto.c）

| 用例 | 结果 |
|---|---|
| CRC check 值 0x29B1 / NULL 防护 | ✅ |
| 编码↔解析回环（4B/0B/64B 载荷） | ✅ |
| LEN>64 → FMT_ERR；VER 错 → VER_ERR；CRC 篡改 → CRC_ERR | ✅ |
| 垃圾字节后再同步 | ✅ |
| encode/build 参数校验（NULL/容量/非法 LEN） | ✅ |
| 遥测编解码回环（含负速度、边界 u32） | ✅ |
| 10⁷ 随机字节模糊（不死机不越界） | ✅ |
| 5 万轮随机帧 + 1/3 概率单比特篡改（无误收） | ✅ |

## 9. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| P-1 | 帧编码/解析/CRC | ✅ | 全部单测覆盖 |
| P-2 | 遥测 LE 编解码 | ✅ | |
| P-3 | 命令常量全集（含 0x70–0x79 DPT） | ✅ | |
| P-4 | TC275 侧采用同一文件 | 🔴 | myCar 仍为 v1（C1 决策：直拷接入，另立任务） |
| P-5 | 握手帧 `{protoVer, fwVer, boardId, capabilities}` 交换 | 🟡 | SDD §6.1 提及；当前 VER 字节即版本协商载体，完整握手帧未实现（波特率握手 0x44 已覆盖链路层协商） |
