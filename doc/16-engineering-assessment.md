# esp32c6_car 固件工程技术评估报告

| 项 | 内容 |
|---|---|
| 文档版本 | V1.0 |
| 日期 | 2026-09-27 |
| 评估对象 | SmartDrive ESP32-C6 网络协处理器固件（`esp32c6_car` 工作区） |
| 评估方式 | 源码通读（启动/状态机、LINK/SF、proto、bridge、OTA/ed25519、pair、http/ws、net）+ 文档核对 + 主流技术基线比对 |
| 代码基线 | ESP-IDF v6.1-beta1，`idf.py build` 通过，`esp32c6_car.bin` ≈ 1.05 MB，8 MB flash |
| 上游基准 | [00-overview.md](00-overview.md)（模块完成状态矩阵） |
| 结论摘要 | 架构与工程质量属**生产级水准**；核心风险集中在"未经 HIL/目标机实测"的验证缺口，以及量产密钥/安全启动的收尾环节 |

---

## 1. 系统定位与架构

这是一个**双板架构中的网络协处理器**：ESP32-C6 负责接入网(softAP/Portal/mDNS)、WebSocket 控制面、配对与会话、双板 OTA 中继；所有安全裁决与运动控制在 TC275(AURIX)侧。C6 只做"第一道门"(角色 + SEQ 单调性)与传输，不做最终授权判定。这个**职责边界划得很清楚**，符合把安全关键逻辑收敛到确定性 MCU、把连接性/协议栈放到富连接 SoC 的主流分工。

架构上最值得肯定的三点：

1. **组合根单点接线(星型依赖)**。`app_main.c` 是唯一进行跨组件回调/sink 注册的地方，各 component 之间不互相 include 业务头。这让依赖图是一棵星而非网，单元可测性和替换性都强。
2. **单写者上下文约束**。文档明确并在代码中落实了"唯一 LINK TX 编排 = bridge_task""唯一 WS 广播 = bridge_task(20ms 节拍)"，从设计上消除了大量并发写竞争。这是嵌入式并发里最省心的模式。
3. **状态派生而非事件假设**。`bridge_reconcile_link_state()` 每 20ms 用 `link_is_up()` 对账，而不是依赖一次性的 `LINK_EV_UP` 边沿事件——这直接规避了双板上电顺序竞态导致"车在线但页面常灰"的经典 bug。这种"状态自愈"的思路是成熟固件的标志。

---

## 2. 分模块技术评估

### 2.1 LINK(SPI 从机 + SF 帧) — 强

- 使用 `spi_slave_hd` 段模式 + 6 字共享寄存器握手 + 开漏 IRQ 数据就绪线 + 500ms 静默看门狗，是 ESP-IDF 上做**高吞吐板间半双工**的正确路子(相比 UART，SPI 从机 + DMA 段是更主流、更高带宽的选择)。
- 防御性细节到位：TX 段 2s stall 自动重臂(master 中途消失不会永久卡死 TX_PENDING)；RX 队列失败重挂；CRC 连错 5 次判 LINK DOWN；SEQ 连续越窗 8 次触发重锁(避免对端重启后静默丢 ~224 帧)。这些都是**只有真机踩过坑才写得出来**的代码。
- ISR 在 `s_link.task==NULL` 时跳过 notify(段 ISR 早于任务创建被 arm)，避免 `vTaskNotifyGiveFromISR` 断言——细节严谨。

### 2.2 proto v2 / SF 编解码 — 强(验证最充分)

- 帧格式 `AA 55 VER CMD SEQ LEN DATA CRC16-CCITT-FALSE`，载荷**显式小端**(TriCore 大端 ↔ RISC-V 小端，禁结构体直转)。跨端字节序显式化是**教科书级正确做法**，规避了最隐蔽的一类跨平台 bug。
- 主机单测覆盖：proto 全分支 + CRC 校验值(0x29B1)+ 10⁷ 随机帧模糊。这是全项目验证最扎实的模块(✅ 100%)。

### 2.3 OTA 自升级 + ed25519 验签 — 强(安全设计正确)

安全关键路径的设计**方向完全正确**：

- **先验签后落盘**：`bundle_parse_header` 在写任何 flash 前，对头部 84 字节做 ed25519 验签；写入尺寸取**已签名的 `c6_len`** 而非不可信的 `total` 字段——防止越界写。
- **流式 SHA-512 摘要校验** + A/B 双槽 + `pending-verify` 回滚(45s 自检窗口，`link_is_up` 作为确认条件之一)。
- ed25519 为**从零实现的 RFC 8032 verify-only**(8×32 limb 域运算，曲线常数由 `gen_crypto_consts.py` 生成而非手敲，注释明确"仅处理公开数据故非常量时间")。有 RFC 8032 正/反测试向量 + dev 密钥端到端单测。
- UAF 防护严谨：`ota_self_begin` 只在 `task_exited` 证实后才回收上一会话的 q/done/bundle；`finish` 超时后宁可留会话给下次 begin 回收，也不在 ota_task 可能仍在触碰资源时 free。

> ⚠️ **注意**：自己实现密码学在业界通常被劝退。这里的选择在**验签-only、公钥数据、有 RFC 向量**的前提下是可辩护的，但仍建议(见第 4 节)与 IDF 内置 `mbedtls`/`esp_secure_boot` 的验签做交叉对拍，并纳入模糊测试。

### 2.4 bridge(命令泵/遥测广播/OTA 中继) — 较强

- 三台泵共用一个 20ms 节拍 + 队列集(`xQueueSelectFromSet`)，命令/RX/事件/定时统一在一个任务里处理，并发模型干净。
- **遥测单槽邮箱"最新赢"**：高频遥测不排队堆积，只留最新——对 50Hz 仪表这是对的。
- DRIVE 命令满队列时丢最旧取最新(周期性、兼作心跳)，一次性命令(pair/OTA/DPT)则严格不丢报 busy——**这个区分很专业**。
- OTA 中继 8×信用窗 + 累积 ACK + 2s 超时重发 + 锁序(relay_mtx → link tx_mtx，信用等待在锁外)。锁序显式声明并落实，死锁风险低。

### 2.5 pair / 会话 token — 较强

- token 32B 随机，**线上传 hex、盘上只存 SHA-256 前 16B 哈希**，NVS 保留 30s 宽限(页面刷新/C6 重启可无按钮重连)。不落明文 token 是正确的。
- 配对窗口单发消费；`pair_request` 前 drain 掉迟到的 CONFIRM 避免误裁决——竞态处理细致。

### 2.6 http / ws / net — 中等偏强(功能全，细节多为真机踩坑修复)

- WS 预握手做 token→角色鉴权，且明确"101 响应前不得成为广播目标"(否则帧插入握手字节导致浏览器拒绝升级)——这是很深的坑，注释详实。
- 大量**运维级修复**：静默断连靠内核 keepalive(5s idle/2s×2 探测)回收 socket；`TCP_MSL=10s` 压缩 TIME_WAIT 防 pcb 池耗尽；连接 fd 清单诊断(SO_TYPE+getpeername)为"页面打不开"事故留证据。这些说明**已经在真机上跑过并解决了实际问题**。
- captive portal 通配兜底重定向到 `192.168.4.1`，触发 iOS/Android 弹窗。net 层注意到 IDF 6.x softAP 不再发 `IP_EVENT_AP_GOT_IP`，用静态 IP 兜底——**跟进了新版本行为变化**。

---

## 3. 与主流技术路线的对照

| 维度 | 本项目做法 | 主流/推荐做法 | 评价 |
|---|---|---|---|
| SoC 选型 | ESP32-C6(RISC-V, Wi-Fi6/BLE5/Thread) | C6 是 Espressif 当前主推的连接协处理器 | ✅ 选型前瞻，不过时 |
| SDK 版本 | ESP-IDF v6.1-beta1 | 当前稳定为 v6.x(v6.0 于 2026-03 发布) | 🟡 用了 beta，量产前应钉到 GA 稳定版 |
| 板间传输 | SPI slave-HD 段模式 + SF 帧 | 高带宽板间首选 SPI/DMA | ✅ 正确，优于 UART 路线 |
| Web 控制面 | httpd + 原生 WebSocket 二进制帧 | ESP 上事实标准 | ✅ 主流 |
| OTA | A/B 双槽 + 回滚 + ed25519 验签 | 主流(通常用 mbedTLS/secure boot 验签) | 🟡 方向对，自研密码学需额外背书 |
| 跨端字节序 | 显式小端序列化 | 强类型跨平台协议标准做法 | ✅ 教科书级 |
| 并发模型 | 单写者 + 队列集 + esp_timer 节拍 | FreeRTOS 上稳健模式 | ✅ 干净 |
| 安全启动/Flash 加密 | NVS 加密已开；secure boot/eFuse 熔断标注为产线环节 | 量产需 secure boot v2 + flash enc | 🟡 收尾在产线，当前未闭环 |
| 测试 | 主机单测(纯 C99)覆盖 proto/sha512/ed25519/bundle/sf | 单元 + HIL 分层 | 🟡 单元层扎实，HIL 层(G4)未开始 |

一句话：**技术路线选择全部踩在当前主流/前瞻的点上**，没有过时或走偏的地方。真正的差距不在"选型"，而在"验证成熟度"和"量产安全收尾"。

---

## 4. 主要风险与建议(按优先级)

**P0 — 量产前必须闭环**

1. **换产线密钥对**。当前验签公钥是台架 dev key(`pub_ed25519_dev.bin`，私钥种子在仓库内)。量产前必须换产线密钥，且私钥不得入库。(README 已声明，须落实。)
2. **关闭 DEV 兜底**。`CONFIG_C6_FACTORY_DEV_OVERRIDE` 量产必须为 `n`，否则无出厂资料也能以固定 SSID/密码启动。
3. **启用 secure boot v2 + flash 加密**。当前仅 NVS 加密；固件本体与验签公钥在明文 flash 上，验签可被绕过(改公钥即可)。安全 OTA 的信任根必须落在 secure boot 上，否则验签只是"防误刷"而非"防攻击"。

**P1 — 验证成熟度**

4. **补齐 HIL/目标机验证(G4)**。文档诚实标注了大量 🟩"编译通过/代码完成、待 HIL"：LINK 波形兼容与 5MHz 台架门禁、OTA 全流程目标机验证、bridge 信用窗时序、配对端到端联调。这些是当前**最大的未知**——设计看起来对，但没在真硬件上跑通闭环。
5. **自研 ed25519 增加背书**。与 IDF `mbedtls` 的 ed25519 做**差分对拍**(同输入同结论)，并对 `bundle_feed`/`ge_unpack` 做模糊测试(畸形头、截断、非规范 s、非平方点)。fe_reduce 的"3 轮条件减"边界(注释称 r < 2p+38)建议加断言/形式化核对。

**P2 — 稳健性收尾**

6. **BLE DPT 通道(c6_maint)仅 55% 骨架**。产测通道(FR-7)未实现且默认关；若产线依赖它写出厂数据/uplink，需排期补齐。factory 写入入口(DPT)未接同属此项。
7. **legacy TCP 桥未测**。默认关，代码完成未验证；若不交付建议明确裁剪(⚪)而非留半成品。
8. **版本钉稳定版**。ESP-IDF 从 v6.1-beta1 迁到 GA 稳定分支，避免 beta 期 API/行为漂移。

---

## 5. 综合结论

**这是一份质量明显高于一般 ESP32 项目的固件。** 架构边界清晰、并发模型自律、防御性编程扎实、文档与代码状态自洽且诚实(状态矩阵敢标 🔴/🟡)。大量注释记录了真机踩坑与修复，说明它不是纸面设计而是**跑过、调过**的工程。

它离"可量产"还差的**不是设计，而是两类收尾**：

- **安全信任根闭环**：secure boot + 产线密钥 + 关 dev 兜底(P0)；
- **目标机验证闭环**：LINK 波形/OTA 全流程/配对端到端的 HIL(P1)。

在这两类闭环完成前，建议定位为**"设计与实现完成、待硬件在环验证的预量产固件"**。核心技术路线无需返工，推进重心应放在验证与安全收尾，而非架构调整。

---

## 附：证据索引(已通读源码)

- 启动/状态机：`main/app_main.c`、`main/app_state.c`(堆守护、45s 回滚自检、diag 聚合)
- LINK/SF：`components/c6_link/link.c`(SPI slave-HD、SEQ 重锁、TX stall 重臂)
- 协议：`components/c6_proto/proto_frames.c`、[14-sf-link.md](14-sf-link.md)(v2↔SF 映射)
- 桥接：`components/c6_bridge/bridge.c`(三泵、信用窗、状态自愈)
- OTA/密码：`components/c6_ota/{ota_self,bundle,ed25519v}.c`(先验签后落盘、流式 SHA-512、RFC8032)
- 配对：`components/c6_pair/pair.c`(token 哈希存储、30s 宽限)
- Web/网络：`components/c6_http/http_server.c`、`components/c6_net/net.c`(WS 鉴权、keepalive 回收、captive portal)
- 测试：`test/host/`(proto/sha512/ed25519/bundle/sf，C99 主机单测)
- 配置：`sdkconfig.defaults`、`partitions.csv`(NVS 加密、回滚、coredump、双槽分区)
