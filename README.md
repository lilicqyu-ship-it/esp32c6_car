# c6_car — SmartDrive ESP32-C6 网络协处理器固件

softAP + WebSocket + 配对 + 双板 OTA 中继的 C6 侧固件（proto v2、CRC16、安全性裁决全部在 TC275）。
设计文档：`../myCar/doc/esp32c6-fw-design.md`（LLDD）、编码计划：`../myCar/doc/esp32c6-fw-coding-plan.md`。
**模块详细设计与完成状态：[`doc/`](doc/00-overview.md)**（每模块一份：架构/接口/时序/完成状态表）。

## 目录

```
main/            启动编排 app_main + app_state（组合根，唯一接线处）
components/
  c6_proto/      proto v2 编解码（纯 C 单一实现，TC275 侧同文件复用）
  c6_factory/    NVS 出厂数据（SN/密码/通道/预配对表/会话宽限）
  c6_link/       UART1 帧链路：装配/健康监测/921600↔2M 波特率握手/TX 队列
  c6_net/        softAP + Captive DNS(UDP53) + mdns_lite + Wi-Fi 事件
  c6_http/       httpd + WS 会话表 + assets 分区流式服务 + REST/OTA 端点
  c6_pair/       配对窗口跟随 + 会话 token（SHA-256 截断存储）+ 30s 宽限
  c6_bridge/     三台泵：命令/遥测广播(50Hz 邮箱+慢客户端降频)/OTA 中继(8×512B 信用窗)
  c6_ota/        自身 A/B：bundle 流式解析 + ed25519 验签(仅验签) + assets 更新 + 回滚
  c6_maint/      BLE DPT（CONFIG_C6_MAINT_BLE，默认关）
  c6_legacy/     TCP 8080 直通桥（CONFIG_C6_LEGACY_TCP，默认关）
assets_src/      控制页源码（摇杆 + 50Hz 仪表 + 配对 + 双板 OTA）
tools/           build_assets.py · sign_bundle.py · ed25519_ref.py · gen_crypto_consts.py
test/host/       主机单测（proto 模糊 10^7 / sha512 / ed25519 RFC8032 / bundle）
```

## 构建（ESP-IDF v6.1-beta1）

```powershell
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

- 台架开发默认 `CONFIG_C6_FACTORY_DEV_OVERRIDE=y`：无出厂资料时以 `SD-DEV000` /
  `sddev123456` 启动。**量产必须设为 n**（严格走 FACTORY_WAIT）。
- 日志默认 WARN；台架可在 menuconfig 调高。

## 控制页 assets（可选，但建议）

空 assets 分区时 `/` 回退固件内嵌极简页。正式页面：

```bash
python tools/build_assets.py                     # assets_src → build/assets.bin
parttool.py -p PORT write_partition --partition-name=assets --input build/assets.bin
```

## 固件签名（/ota/c6）

验签公钥内嵌于 `components/c6_ota/keys/pub_ed25519_dev.bin`（对应私钥种子
`tools/keys/ed25519_dev.seed`，仅台架用）。**量产前必须换产线密钥对**：

```bash
python tools/sign_bundle.py --c6 build/c6_car.bin --out build/c6fw.bundle
curl -F file=@build/c6fw.bundle "http://192.168.4.1/ota/c6?token=<控制端token>"
```

TC275 固件经 `POST /ota/tc275` 由 c6_bridge 信用窗口中继（0x60–0x65，LLDD §4.6.3）。

## 主机单测（G1 门）

```bash
cd test/host
make check          # 或用任意 C99 编译器按 Makefile 里的四条 gcc 命令
```

覆盖：proto 全分支 + CRC check 值(0x29B1) + 10^7 随机帧模糊；SHA-512 已知向量；
ed25519 RFC 8032 正/反向量 + dev 密钥端到端；bundle 签名/哈希/越界/截断。

## 关键口径（与 LLDD 的差异见编码计划 C1–C10）

- 帧格式 `AA 55 VER(02) CMD SEQ LEN DATA CRC16-CCITT-FALSE`，LEN ≤ 64
- 载荷一律显式小端（TriCore 大端 ↔ RISC-V 小端，禁止结构体直转）
- `0x63` = OTA_STATUS（`state==DONE` 即 END），`0x64` SWAP、`0x65` ABORT
- 会话 token 32B 随机，仅存 SHA-256 前 16B 哈希（内存 + NVS 30s 宽限）
- LINK 引脚候选 GPIO10/11（`components/c6_link/Kconfig`，EE 评审 Q1 后改配置）
