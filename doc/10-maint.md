# 10 BLE DPT 产测通道（c6_maint）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_maint/maint_ble.c`、`include/maint_ble.h` |
| 上游需求 | LLDD §4.9、§3.1（DPT 纪律）、FR-7；编译开关 `CONFIG_C6_MAINT_BLE`（默认 n，决策 C7） |
| 状态 | 🔴 **55%（骨架）** — GATT 骨架 + LINK 透传完成；**未经编译**（默认关闭）、C6 本地 DPT 项未实现 |

## 1. 职责（目标态）

产线治具经 NimBLE GATT 执行 DPT 序列（SDD §10.2）：
版本/自检/SN 写/预配对/老化启停为 **C6 本地项**；运动类 0x7x 透传 LINK。

## 2. GATT 布局（实现态）

```
Service  0xFD6C-D123-A18F-485B-B48E-1E43-FDD1-0711（产测专用 128-bit UUID）
  Char CMD（写）  20–256 B：JSON/proto 帧
  Char IND（指示）响应/日志回传
广播名 "SD-DPT"，公开地址，120–180 ms 间隔
```

## 3. 会话安全（LLDD §10.1 治具令牌）

连接后**首帧**必须是 `DPT_ENTER(0x70)` 帧（AA 55 … 0x70 LEN=16）携带 16 B
治具令牌（MES 下发）——校验通过才会话打开；未开会的后续写一律
`INSUFFICIENT_AUTHEN` 拒绝。令牌比对目前是"接收并打开会话"，**未与预存
令牌表核对**（MES 侧令牌分发机制未定稿）。

## 4. 透传路径（实现态）

开会后，后续 CMD 帧按 proto 帧解出 `cmd/len/data` → `link_send()` 直透
（0x72 MOTOR_RUN、0x73 ENC_READ、0x77 AGING 等由 TC275 执行）。

## 5. 已实现 / 未实现清单

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| M-1 | NimBLE 初始化 + 广播 + GATT 注册 | 🟡 | 代码完成；**默认构建不编译**，从未过编译器（需 `CONFIG_BT_ENABLED=y` + `CONFIG_C6_MAINT_BLE=y` 构建） |
| M-2 | 治具令牌门禁 | 🟡 | 开会流程实现；未与令牌表核对 |
| M-3 | C6 本地 DPT 项（版本/自检/SN 写/预配对/老化启停 → c6_factory/c6_ota） | 🔴 | LLDD §3.1 明确 C6 本地处理；当前全部透传，未分派 |
| M-4 | IND 响应回传（LINK 0x7x 应答 → IND） | 🔴 | 未接（LINK → BLE 方向无路径） |
| M-5 | Wi-Fi DPT 回退（专用 SSID `SD-xxxx-DPT`，Q3 消费版无 BLE 时） | 🔴 | 未实现 |
| M-6 | DPT 进入后禁用运动面（页面/配对限制） | 🔴 | 未实现（依赖 app_state FACTORY_WAIT 收紧，01 文档 S-4） |

## 6. 风险与建议

1. **编译风险未排**：首次启用编译预计需处理 NimBLE 头文件/链接配置（`bt` 组件 REQUIRES 已预置）；
2. 建议 M-3/M-4 与产测治具协议联一起做（依赖 SDD §10.2 命令集定稿）；
3. 令牌核对表落在 c6_factory（`ppt` 键族或新增 `dpt_tok`），机制同 03 文档 F-5。
