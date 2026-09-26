# 13 验证与测试状态汇总

| 项 | 内容 |
|---|---|
| 代码位置 | `test/host/`（Makefile、minunit.h、4 个测试程序） |
| 上游需求 | LLDD §9（测试设计）、编码计划 §5（G1–G4 门） |
| 状态 | 🟡 — **G1 ✅（33/33）/ G2 ✅ / G3 ✅（走查）/ 22 §8 台架门禁 🔴 / G4 🔴** |

## 1. 验证门总览

| 门 | 内容 | 通过标准 | 状态 | 结果 |
|---|---|---|---|---|
| G1 | 主机单测 | 全绿，proto/SF 100% 分支 | ✅ | 33/33（见 §2） |
| G2 | 目标编译 | 0 error / 自研组件 0 新增 warning | ✅ | `idf.py build` exit=0，`c6_car.bin` 1,072,624 B |
| G3 | 静态走查 | 无回调内耗时/无未校验 memcpy/ISR 禁锁 | ✅ | 随编码完成；本轮文档梳理另修 2 缺陷（07 文档 §8） |
| G4 | HIL 台架 | LLDD §9 集成/压力/老化 | 🔴 | 未开始（需双板台架） |

## 2. G1 主机单测清单（test/host，`make check`）

### test_proto（12 项，覆盖 c6_proto 100% 分支）
CRC check 值 0x29B1 · NULL 防护 · 编码回环（4B/0B/64B）· LEN 越界 FMT_ERR ·
VER 错 VER_ERR · CRC 篡改 CRC_ERR · 垃圾再同步 · encode/build 参数校验 ·
遥测编解码回环 · **10⁷ 随机字节模糊** · **5 万轮随机帧+单比特篡改（无误收）**

### test_sha512（4 项）
"abc"（FIPS 例）· 空串 · FIPS 长消息 · 流式 vs 一次性一致（向量经 Python
hashlib 生成核对）

### test_ed25519（4 项）
RFC 8032 向量 1（空消息）· 向量 2（0x72）· dev 密钥端到端（tools/ed25519_ref.py
签名 ⇄ C 验签）· 坏公钥/非规范 s 拒绝

### test_bundle（6 项）
真实签名头逐字节流 · 坏 magic · 坏签名 · 错公钥 · 尺寸不符 · 截断检测

### test_sf（7 项，SF 帧 = SPI 链路容器，myCar doc 22 §5）
编解码回环 · 段内多帧 + 4B 补零 · 错误分支（CRC/VER/LEN 越界/垃圾再同步）·
SEQ 严格前进窗口（1..32 含回绕）· 10⁷ 随机字节模糊 · v2↔SF 映射回环
（DRIVE/LINK_STATE/未映射命令）· OTA CHUNK 242B 帧布局（4B 对齐）

## 3. 未实施的单测（LLDD §9 差距）

| 项 | 原因 | 计划 |
|---|---|---|
| 会话表状态机主机测（LLDD §9 层 1） | ws_sessions 依赖 FreeRTOS 互斥，未做抽象 | 抽出无锁核心或上 POSIX 桩 |
| IDF pytest-embedded 目标单测（c6_link 装配/健康、pair 窗口、遥测降频） | 需目标机/模拟环境 | 与 HIL 台架同批 |
| 压力（4 客户端 + 慢客户端 + WS 洪泛 + 上传反复断连） | 同上 | G4 |
| 老化（72h 遥测 + 周期配对 + 30min OTA） | 同上 | G4 |

## 4. G4 HIL 用例清单（从 LLDD §9 与模块文档汇总）

| 用例 | 出处 | 通过标准 |
|---|---|---|
| **22 §8 G1 波形兼容**（TC275 数据字节模拟 CMD/ADDR/DUMMY） | doc 22 R7 | C6 侧 RD_REG/WRDMA 拿到正确字节、事件按预期触发（失败退路：自写从机驱动 / 回退 UART） |
| 22 §8 G3 链路时延 | doc 22 | 触屏→TC275 收到命令最坏 ≤5 ms（含 IRQ 丢失走 2ms 兜底） |
| 22 §8 G4 安全语义 | doc 22 | 拔 C6 电源/断 IRQ/GEN 静默 → ERR_LINK_LOST ≤520 ms、受控停车 ≤100 ms |
| 22 §8 G5 提速阶梯 1→2→5→10→20 MHz | doc 22 | 每档 30 min CRC 误码 0；OTA 1 MB ≤3 s @5 MHz |
| 22 §8 G6 老化 | doc 22 | 72 h 0 意外复位、链路错误计数不增长 |
| 断链 LINK_STATE 时序（拔线） | 08-bridge / FR-5 | ≤10 ms 发出 0x42 |
| 命令端到端时延 | FR-2 | WS→轮 ≤50 ms（SDD 预算） |
| OTA 中继全流程 + 每阶段断电注入 | 08/09 | 回滚 100/100 |
| 自身 OTA 全流程 + PENDING_VERIFY 断电 | 09/01 | 回滚成功、/api/health 可见槽位 |
| 配对窗口/宽限/重连 | 06-pair | LLDD §4.4 时序 |
| Portal 弹窗 + mycar.local 解析 | 05-net | iOS/Android 实机 |
| 4 客户端并发 + 慢客户端降频 | 07-http | 控制端时延不劣化、无堆耗尽 |
| 72 h 老化 | LLDD §9 | 0 泄漏（水位不降）、0 复位 |

## 5. 复现步骤

```bash
# G1（任意 C99 编译器；MSYS2 mingw64 gcc 实测通过）
cd c6_car/test/host && make check

# G2（ESP-IDF v6.1-beta1）
cd c6_car && idf.py build
```

## 6. 台架弱电源缓解（真机 bring-up 记录，2026-09）

台架电源在 Wi-Fi 上电（PHY 校准电流峰，约开机 1 s 处）会瞬间跌落到
ESP32-C6 最低欠压阈值（SEL_7 = 2.51 V）以下，触发 brownout 复位循环。
`main/Kconfig.projbuild` 提供三个台架专用缓解项（生产构建全部保持默认值）：

| Kconfig | 默认 | 作用 |
|---|---|---|
| `C6_BENCH_BOD_DISABLE` | n | app_main 最早处调用 `esp_brownout_disable()`（闪写/RF 校准脱离保证电压窗口，仅台架） |
| `C6_NET_START_DELAY_MS` | 0 | Wi-Fi 启动前延时，让电源从开机浪涌恢复（台架取 300） |
| `C6_WIFI_TX_POWER_QDBM` | 0 | 封顶 TX 功率压低 PA 电流峰，0.25 dBm 单位（台架取 48 = 12 dBm） |

注意：ESP32-C6 的欠压阈值阶梯是**降序**的（SEL_7 = 2.51 V 最低，
SEL_2 = 3.27 V 最高），`sdkconfig.defaults` 中不要写 `..._SEL_2_5V`
（那是 C3/S 系的写法，对 C6 无效）。

串口抓取辅助工具：`tools/serial_sniff.py`（pyserial，复位 + 带时间戳
打印启动日志）：

```bash
idf.py -p COM14 flash
python tools/serial_sniff.py COM14 30
```
