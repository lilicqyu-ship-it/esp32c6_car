# esp32c6_car — SmartDrive ESP32-C6 网络协处理器固件

softAP + WebSocket + 配对 + 双板 OTA 中继的 C6 侧固件（proto v2、CRC16、安全性裁决全部在 TC275）。
控制端两类客户端，协议面完全一致、零区分对待：手机 Web 控制页（`assets_src/`，烧入 assets 分区）
与 ESP32-S3 LCD 遥控器（平级仓库 [`../smartcar_remote`](../smartcar_remote)，2026-09-29 起，
proto v2 编解码原样复用本仓 `c6_proto/proto_frames.[ch]`，规格书见其 `doc/`）。
上游接口基准（在 tc275_car 仓库）：[21-software-design.md](../tc275_car/doc/20-design/21-software-design.md)（量产 SDD，设计基准）、[22-link-spi-design.md](../tc275_car/doc/20-design/22-link-spi-design.md)（板间 SPI/SF 帧）、[41-c6-docs-map.md](../tc275_car/doc/40-esp32c6/41-c6-docs-map.md)（两仓库文档分工 + 跨仓 TODO T1–T6）。
**模块详细设计与完成状态：[`doc/`](doc/00-overview.md)**（每模块一份：架构/接口/时序/完成状态表）。

## 目录

```
main/            启动编排 app_main + app_state（组合根，唯一接线处）
components/
  c6_proto/      proto v2 编解码（纯 C 单一实现，TC275 侧同文件复用）
  c6_factory/    NVS 出厂数据（SN/密码/通道/预配对表/会话宽限）
  c6_link/       LINK 传输层：SPI 从机(spi_slave_hd 段模式) + 共享寄存器握手
                 + IRQ 数据就绪线 + 健康监测；v2↔SF 帧映射内置于 link
  c6_sf/         SF 帧编解码（SPI 链路专用容器，纯 C 主机可测，tc275_car doc 22 §5）
  c6_net/        softAP + Captive DNS(UDP53) + mdns_lite + Wi-Fi 事件
  c6_http/       httpd + WS 会话表 + assets 分区流式服务 + REST/OTA 端点
  c6_pair/       配对窗口跟随 + 会话 token（SHA-256 截断存储）+ 30s 宽限
  c6_bridge/     三台泵：命令/遥测广播(50Hz 邮箱+慢客户端降频)/OTA 中继(8×512B 信用窗)
  c6_ota/        自身 A/B：bundle 流式解析 + ed25519 验签(仅验签) + assets 更新 + 回滚
  c6_maint/      BLE DPT（CONFIG_C6_MAINT_BLE，默认关）
  c6_legacy/     TCP 8080 直通桥（CONFIG_C6_LEGACY_TCP，默认关）
  c6_adxl345/    ADXL345 三轴加速度计（GPIO 位拍 SPI，仅 /diag 本地传感器，doc 18）
assets_src/      控制页源码（摇杆 + 50Hz 仪表/车速表 + 配对 + 双板 OTA）
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

## build 产物与分区（8 MB flash）

`idf.py build` 产出（`build/`）：

| 产物 | 内容 | 去向 |
|---|---|---|
| `bootloader/bootloader.bin` | 二级引导 | `0x0` |
| `partition_table/partition-table.bin` | 分区表 | `0x8000` |
| `ota_data_initial.bin` | OTA 槽位标记初始值（指向 ota_0） | `0x19000` |
| `esp32c6_car.bin` | **应用固件**（烧入 ota_0/ota_1 槽） | `0x20000`（ota_0） |
| `assets.bin` | 控制页打包件（`tools/build_assets.py` 生成） | assets 分区 `0x620000` |
| `esp32c6_car.elf` / `esp32c6_car.map` | 调试符号/链接映射（panic 解栈、addr2line 用），**不烧录** | — |
| `*_flashed.bin` | 增量烧录差分缓存（esptool `--diff-with`），**勿手动烧** | — |

分区布局（真源 `partitions.csv`，LLDD 2.1）：`ota_0`/`ota_1` 各 3 MB 双槽
（A/B 升级+回滚），`assets` 512 KB 控制页，`coredump` 64 KB 崩溃转储，
`factory_ota_cache` 1 MB TC275 固件中继缓存，`nvs/nvs_cert/nvs_keys` 出厂
与配对数据（nvs_keys 加密）。

## 烧录方法

按目标分四种，均走 USB 串口（板载自动下载电路，**无需按 BOOT 键**，烧完自动复位运行）。

> **捷径**：不想激活 IDF 环境时，在普通 CMD / 资源管理器里直接运行项目根目录的
> `flash.bat`（bat 版）或 `python flash.py`（click 命令行版，EIM 元数据自动
> 发现环境、缺 click 时自动切到 IDF venv 的 Python）。子命令：
>
> ```
> python flash.py build         # 编译（idf.py build，含 assets 自动打包）
> python flash.py               # full（默认）：全量四件套（bin 缺失时自动先编译）
> python flash.py assets        # 仅重打包并烧控制页，固件不动
> python flash.py all -b -m     # 先 assets 后固件，-b 烧前编译，-m 开串口监视
> python flash.py mon           # 仅串口监视
> ```
>
> 各烧录命令均支持 `-p COM7` 指定端口（省略时按 USB VID 自动识别：
> Espressif 0x303A 优先，CP210x/CH34x/FTDI 桥接次之）、`-b` 烧前编译、
> `-m` 烧后监视。`idf.py` 只有在已激活环境的 PowerShell 里才可用。

```powershell
# ① 全量烧录（新板 / 首次 / 分区表改动后）：引导 + 分区表 + otadata + 固件
idf.py -p PORT erase-flash          # 可选：整片擦除（连带清 NVS 配对/宽限）
idf.py -p PORT flash

# ② 日常开发（改了固件代码）：同一命令，esptool 只写变化扇区，秒级
idf.py -p PORT flash

# ③ 仅更新控制页（改了 assets_src/，固件不动）
python tools/build_assets.py assets_src build/assets.bin
parttool.py -p PORT write_partition --partition-name=assets --input build/assets.bin

# ④ OTA 槽位复位（升级卡死/回滚循环时）：重写 otadata 回到 ota_0
parttool.py -p PORT write_partition --partition-name=otadata --input build/ota_data_initial.bin
```

免 IDF 环境（如产线治具）：任意 Python + esptool 直接按偏移烧四件套：

```bash
python -m esptool --chip esp32c6 -p PORT -b 460800 write-flash @build/flash_args
```

（`flash_args` 即 build 目录生成的偏移清单，与 `idf.py flash` 等价。）

量产现场的固件更新走网页 OTA（`/ota/c6`，带 ed25519 验签与 A/B 回滚），见下节。

## 控制页 assets（可选，但建议）

空 assets 分区时 `/` 回退固件内嵌极简页。正式页面：

- `idf.py build` **已自动打包**：assets_src/* 变化时增量产出 `build/assets.bin`
  （根 CMakeLists 的 `c6_assets` 目标）；
- 烧录是独立步骤（写 assets 分区）：

```bash
parttool.py -p PORT write_partition --partition-name=assets --input build/assets.bin
# 或直接用一键工具：flash.bat assets / python flash.py assets
```

（脱离 IDF 环境时手动打包：`python tools/build_assets.py assets_src build/assets.bin`）

## 固件签名（/ota/c6）

验签公钥内嵌于 `components/c6_ota/keys/pub_ed25519_dev.bin`（对应私钥种子
`tools/keys/ed25519_dev.seed`，仅台架用）。**量产前必须换产线密钥对**：

```bash
python tools/sign_bundle.py --c6 build/esp32c6_car.bin --out build/c6fw.bundle
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

- 手机/WS 侧帧：`AA 55 VER(02) CMD SEQ LEN DATA CRC16-CCITT-FALSE`，LEN ≤ 64；
  板间 SPI 链路用 SF 帧（`5A 01 TYPE SEQ FLAGS LEN CID DATA CRC16`，LEN ≤ 248，
  4B 段对齐），映射表见 `doc/14-sf-link.md`
- 载荷一律显式小端（TriCore 大端 ↔ RISC-V 小端，禁止结构体直转）
- `0x63` = OTA_STATUS（`state==DONE` 即 END），`0x64` SWAP、`0x65` ABORT；
  UART 时代的 0x43 PING / 0x44 BAUD 已删除（doc 22 T2）
- 会话 token 32B 随机，仅存 SHA-256 前 16B 哈希（内存 + NVS 30s 宽限）
- LINK 接线（真源 tc275_car 23-wiring §9.1）：SCLK=19 / MOSI=18 / MISO=20 /
  CS=23 / IRQ=21（开漏，10k 上拉为外部件）；量产时钟 5 MHz（22 §8 G5）
