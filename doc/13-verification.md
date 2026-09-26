# 13 验证与测试状态汇总

| 项 | 内容 |
|---|---|
| 代码位置 | `test/host/`（Makefile、minunit.h、4 个测试程序） |
| 上游需求 | LLDD §9（测试设计）、编码计划 §5（G1–G4 门） |
| 状态 | 🟡 — **G1 ✅ / G2 ✅ / G3 ✅（走查）/ G4 🔴（HIL 未开始）** |

## 1. 验证门总览

| 门 | 内容 | 通过标准 | 状态 | 结果 |
|---|---|---|---|---|
| G1 | 主机单测 | 全绿，proto 100% 分支 | ✅ | 26/26（见 §2） |
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
| LINK 波特率握手（30s 升 2M）/误码降速 | 04-link | 30 min 误码 0；降速自动恢复 |
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
