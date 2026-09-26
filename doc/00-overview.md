# SmartDrive C6 固件 — 模块设计文档总览与完成状态矩阵

| 项 | 内容 |
|---|---|
| 文档版本 | V1.0 |
| 日期 | 2026-09-26 |
| 上游 | [21-software-design.md](../../myCar/doc/20-design/21-software-design.md)（量产 SDD，**设计基准**）→ [22-link-spi-design.md](../../myCar/doc/20-design/22-link-spi-design.md)（板间 SPI/SF 帧详细设计）→ [41-c6-docs-map.md](../../myCar/doc/40-esp32c6/41-c6-docs-map.md)（两仓库文档分工与跨仓 TODO）→ 本目录（各模块详细设计 + 完成状态） |
| 代码基线 | `c6_car` 工作区（`idf.py build` 通过，`build/c6_car.bin` ≈ 1.05 MB） |

## 状态图例

| 标记 | 含义 |
|---|---|
| ✅ | 完成：已实现 **且** 经主机单测或编译级验证 |
| 🟩 | 完成：已实现，待目标机/HIL 验证 |
| 🟡 | 部分：主体完成，存在明确交付缺口（在文档内逐条列出） |
| 🔴 | 未实现 |
| ⚪ | V1.0 明确裁剪（LLDD"明确不做"或编码计划决策） |

## 模块索引与完成状态矩阵

| 文档 | 模块 | 代码位置 | 需求追溯 | 完成度 | 验证状态 |
|---|---|---|---|---|---|
| [01](01-app-state.md) | 应用状态机与启动编排 | `main/app_main.c` `main/app_state.c` | LLDD §4.1/§4.10 | 🟡 90% | 🟩 编译通过；link_task TWDT 已补 |
| [02](02-proto.md) | proto v2 编解码（手机/WS 侧） | `components/c6_proto/` | SDD V1.2 §6.1b / LLDD §3.1 | ✅ 100% | ✅ 主机单测 12 项 + 10⁷ 模糊 |
| [03](03-factory.md) | 出厂数据（NVS） | `components/c6_factory/` | LLDD §4.8 | 🟡 90% | 🟩 编译通过；写入入口（DPT）未接 |
| [04](04-link.md) | LINK 链路（**SPI 从机**） | `components/c6_link/` `components/c6_sf/` | myCar doc 22 / SDD V1.2 §6.1a / FR-3 | 🟩 代码完成 | 🟩 G1/G2 通过；波形兼容与台架门禁待测 |
| [05](05-net.md) | 接入网 | `components/c6_net/` | LLDD §4.2 / FR-1 | 🟡 90% | 🟩 编译通过；Portal/mDNS 待真机确认 |
| [06](06-pair.md) | 配对与会话 | `components/c6_pair/` | LLDD §4.4 / FR-4 | 🟩 100% | 🟩 编译通过，流程未联调 |
| [07](07-http.md) | Web 服务 | `components/c6_http/` | LLDD §4.3 / §3.2 / FR-2 | 🟡 92% | 🟩 编译通过；HELLO/abort 缺陷已修待回归 |
| [08](08-bridge.md) | 三台泵 | `components/c6_bridge/` | LLDD §4.6 / §2.4 | 🟩 97% | 🟩 编译通过；信用窗时序待 HIL |
| [09](09-ota.md) | 自身升级与验签 | `components/c6_ota/` | LLDD §4.7 / FR-6 | 🟡 92% | ✅ 验签/解析主机单测；OTA 流程待目标验证 |
| [10](10-maint.md) | BLE DPT 通道 | `components/c6_maint/` | LLDD §4.9 / FR-7 | 🔴 55% 骨架 | 🔴 未编译（默认关）、C6 本地 DPT 项未实现 |
| [11](11-legacy.md) | TCP 8080 直通桥 | `components/c6_legacy/` | LLDD FR-10 | 🟡 代码完成 | 🔴 未编译（默认关）、未测试 |
| [12](12-assets-tools.md) | 控制页与工具链 | `assets_src/` `tools/` | SDD §11 / LLDD §4.7 | 🟡 90% | ✅ 工具实测；页面待真机 |
| [13](13-verification.md) | 验证与测试汇总 | `test/host/` | LLDD §9 + doc 22 §8 | 🟡 G1 绿 / G2 绿 | G3 走查完毕；G4 HIL 未开始 |
| [14](14-sf-link.md) | **SF 链路详设（SPI 落地）** | `components/c6_sf/` `components/c6_link/` | myCar doc 22 §4–§5 | 🟩 代码完成 | ✅ test_sf 7 项；波形兼容待台架 |
| [15](15-led.md) | WS2812 状态指示灯 | `components/c6_led/` | bring-up 运维需求 | 🟩 代码完成 | 🟩 真机验证（绿心跳=正常） |

## 系统级完成视图

```
需求侧（LLDD §1）                        实现落点                 状态
FR-1  softAP/Portal/mDNS                 05-net                   🟡 (mDNS 简化实现，待真机)
FR-2  WS 服务器 / 50Hz 遥测 / <1ms        07-http + 08-bridge      🟡 (已实现，端到端待联调)
FR-3  LINK（SPI 5M / SF 帧 / 健康）       04-link + 14-sf-link     🟩 (代码完成；22 §8 台架门禁待测)
FR-4  配对                               06-pair                  🟩
FR-5  断链即报 LINK_STATE                 08-bridge + 04-link      🟩
FR-6  自身 OTA + TC275 中继               09-ota + 08-bridge       🟡 (全流程待目标验证)
FR-7  产测通道                           10-maint                 🔴 (骨架)
FR-8  安全(token/防重放/验签)             06/07/09                 🟡 (secure boot/熔断属产线)
FR-9  可靠性(WDT/coredump/堆守护)         01-app-state             🟡 (link 任务未订阅 TWDT)
FR-10 遗留桥                             11-legacy                🟡 (代码完成未验证)
```

## 一致性约束（所有模块文档的公共口径）

1. 帧格式 `AA 55 VER(0x02) CMD SEQ LEN DATA[≤64] CRC16-CCITT-FALSE`，多字节载荷**显式小端**。
2. **唯一 LINK TX 编排上下文 = bridge_task**；pair/link 健康定时器只通过 `link_send()`（内部互斥 + TX 队列）。
3. **唯一 WS 广播上下文 = bridge_task**（20 ms 节拍）；httpd 任务内不做耗时操作。
4. 组合根 = `app_main`：所有跨组件回调/sink 注册只发生在这里（星型依赖规则，LLDD §2.2）。
5. 状态标记口径：文档中的"完成"以**本文档集书写时点的代码**为准（含 HELLO/abort 两处修复后的回归编译）。
