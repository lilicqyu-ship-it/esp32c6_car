# 11 TCP 8080 遗留直通桥（c6_legacy）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_legacy/legacy_tcp.c`、`include/legacy_tcp.h` |
| 上游需求 | LLDD FR-10（SDD §7 风险表遗留兼容）；编译开关 `CONFIG_C6_LEGACY_TCP`（默认 n，决策 C7；Q4 未定稿） |
| 状态 | 🟡 **代码完成，未验证** — 默认构建不编译、零测试 |

## 1. 职责

demo 客户端"零感知"过渡：TCP :8080 上的裸 proto 帧直通到 LINK（及反向镜像），
不解析运动语义、不鉴权（V1.0 过渡定位，Q4 建议仅量产首批开启）。

## 2. 架构（实现态）

```
TCP 客户端 ×2 ──字节流──► proto_parser_feed（v2 逐字节解析，CRC 校验）
                             │ 合法帧
                             ▼
                          link_send()（与其它 TX 共用队列/BUSY 语义）

LINK RX ──link_set_tap 注册的镜像回调──► proto_encode ──send──► 全部 TCP 客户端
          （tap 在 link_rx 任务上下文执行：非阻塞、失败即弃，决策 C8 补充）
```

- 监听任务 `legacy`（prio 4 / 3 KB）：accept → 每客户端 `legacy_cli` 任务（3 KB）；
- 对端表：2 槽（`LEGACY_MAX_PEERS`），互斥保护；断开自动回收；
- **只放行合法 v2 帧**：CRC 不过的字节直接丢弃，不做盲管道（防垃圾灌入 LINK）。

## 3. 与 LLDD 口径的差异

LLDD 描述为"裸协议直通桥，demo 客户端零感知"。demo 客户端运行 proto **v1**
（无 VER 字节、异或 CRC），本实现按 **v2** 解析放行——v1 客户端帧会被
VER_ERR 丢弃。原因：Q4（保留几个版本、v1 兼容翻译）未定稿（编码计划 §1
"本版明确不做"）。若 Q4 决定兼容 v1，需在客户端任务内加 v1→v2 翻译层。

## 4. 接口

```c
esp_err_t legacy_tcp_start(void);   /* app_main 在 CONFIG_C6_LEGACY_TCP=y 时调用 */
```

## 5. 资源

监听任务 3 KB + 每客户端 3 KB；无堆驻留（帧栈上组装）。

## 6. 验证状态与完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| G-1 | TCP 监听/多客户端/断开回收 | 🟡 | 代码完成；lwip socket 路径**未编译过**（默认关） |
| G-2 | v2 帧双向直通 | 🟡 | 同上 |
| G-3 | tap 镜像（LINK→TCP） | 🟡 | 机制就绪（link_set_tap），未验证 |
| G-4 | v1 demo 客户端兼容翻译 | 🔴 | Q4 未定稿，本版不做 |
| G-5 | 鉴权/限流 | ⚪ | 过渡桥无鉴权（LLDD 定位）；量产首批后建议关闭 |

**启用方式**：`idf.py menuconfig` → esp32c6_car project → `C6_LEGACY_TCP=y` 后重编译。
