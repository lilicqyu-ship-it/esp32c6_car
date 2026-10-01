# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 修复
- TC275 断电后遥测缓存仍以 50 Hz 永久重播（`mbox_fresh` 从不清除）——客户端新鲜度看门狗被假帧一直喂着，网页与 S3 SYSTEM 页永远显示车端在线；改为消费制：每帧 SPI 遥测只转发一次，链路掉线即停（S3 的丢帧率/接收速率统计也不再计入重播帧）

## [1.0.0] - 2026-10-01

首个稳定版，对齐 `CMakeLists.txt` PROJECT_VER 1.0.0。

### 新增
- 按需版本查询：WS `{"t":"tcver"}` → SPI DIAG 0x53/0x24 中继至 TC275（200 ms 限流，配合 S3 About 页 tap 刷新）
- TC275 版本信标转发：EVT 0x24/0x25 经 DIAG 隧道广播 `{"t":"tcver"}`（配合 tc275_car 版本上链路）
- ADXL345 三轴加速度计（位拍 SPI），集成进 /diag 诊断页
- 台架标定页 /calib.html 与 EVT 0x22/0x23 解码；标定流程重排为四步 + jog 故障门禁
- 控制页体验：深色座舱主题、iOS 手势加固、车速表与 EMA 平滑、角色可见/回切即连/心跳看门狗
- /diag HTML 诊断页 + socket 表自愈（僵尸回收器）
- 可选 captive portal（`CONFIG_C6_CAPTIVE_PORTAL`，默认关弹窗改手动输网址）
- flash 脚本按 USB VID 自动识别板载串口，串口被占用时早报错

### 变更
- 工程名 c6_car → esp32c6_car，统一仓库名/工程名/产物名
- 统一 LF 换行（.gitattributes），保证 contracts 跨平台逐字节校验一致
- flash.py 跨平台 EIM 环境发现；lwIP 槽位提升到 32
- 启动时先打印上次复位原因
- smartcar_remote（S3 遥控器）登记为第二控制端
- CI：固件资源统计门禁（对齐 smartcar_remote）

### 修复
- WS 发送互斥 + PING/PONG 应答，对接 S3 遥控器
- 控制命令处理防陈旧帧；DRIVE 队满时丢弃新帧，规避 queue-set 幻影项断言重启
- 禁用 Nagle 算法，控制/遥测帧即时发送
- WS CLOSE 帧显式回收会话
- send_json 状态行补完整状态码（仅传原因短语产生 "HTTP/1.1 OK" 非法行，esp_http_client 拒绝解析，S3 /api/health 超时）
- .vscode compile-commands-dir 指向当前检出路径

## [0.1.2] - 2026-09-27

### 修复
- WS 重连退避与 err 提示自动恢复；lwIP TIME_WAIT 由 2 min 缩短到 20 s——修复手机端网页刷新后打不开、控制误报 busy

## [0.1.1] - 2026-09-27

### 新增
- CI：GitHub Actions（主机测试 + 固件编译 + tag 自动发版）

### 修复
- 链路交互简化（SPI 断连类故障根因修复）：SEQ 窗口自愈、TX 卡死重排与 RX 重排重试
- 静默断连的快速回收与 WS 发送反压

## [0.1.0] - 2026-09-27

首个版本化基线（台架联调）。

### 新增
- 板间链路换向 UART → SPI 从机；SPIDBG 链路诊断与复位后首帧序号误判修复
- Web 控制页、captive portal 302 兜底、WS2812 状态指示灯
- 弱电源台架缓解三件套 + 串口抓取工具（真机 bring-up）
- flash.bat / flash.py 一键烧录（full/assets/all 模式）
- 主机单测套件（G1 门 26/26 绿）与 14 份模块设计文档

[未发布]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v0.1.2...v1.0.0
[0.1.2]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/lilicqyu-ship-it/esp32c6_car/releases/tag/v0.1.0
