# 12 控制页与工具链（assets_src / tools）

| 项 | 内容 |
|---|---|
| 代码位置 | `assets_src/`（index.html、style.css、app.js、calib.html、calib.js）、`tools/`（build_assets.py、sign_bundle.py、ed25519_ref.py、gen_crypto_consts.py）、`tools/keys/` |
| 上游需求 | SDD §11（UX 结论）、LLDD §4.7（bundle）、§3.2（端点） |
| 状态 | 🟡 **90%** — 工具链实测可用；页面功能完整但未真机验证；UI 已重构为深色座舱主题（零外部依赖） |

## 1. 控制页（assets_src → assets 分区 / 内嵌回退）

### 1.1 页面功能（app.js，proto v2 的 JS 侧镜像实现）

| 功能 | 实现 |
|---|---|
| WS 接入 | `ws://host/ws?token=`（token 取 sessionStorage / URL 参数），断线 1 s 重连 |
| 摇杆驾驶 | pointer 事件 → `DRIVE(0x50){i16 v, i16 ω}`，30 Hz 定时发送（兼心跳），上=前进 v≤600 mm/s，右转 ω≤300 |
| STOP 按钮 | 发 (0,0) |
| 遥测仪表 | 0x41 载荷逐字段 LE 解码：目标/实测双条（蓝=目标、青=实测，中点居中左负右正）、电量徽章（图标+%+电压，≤10% 红/≤20% 黄）、里程、rtt、故障码人话态（"故障 0x…"，状态行四色 tone：待命绿/错误黄/故障红/未连接灰） |
| 车速仪表 | 车身速度 = (v_meas_l+v_meas_r)/2（带符号平均，原地旋转≈0）→ km/h 一位小数 + 方向行（▲前进/▼倒车/静止，<30 mm/s 判静止）；显示值经 150 ms 时间常数 EMA 平滑（0.1 km/h 数字不跳动、方向行低速不拍），首帧/断流恢复/进出静止直接吸附不爬坡；>1 s 无遥测置灰显示 "--"（链路断裂时不残留旧车速） |
| 配对 | `POST /api/pair` → 200 存 token → 重连 WS 自动成为控制端；403 提示"按车侧键 3 s" |
| 双板 OTA | 文件选择（选中后回显文件名）→ `POST /ota/c6` / `/ota/tc275`（token 附带）；`otastatus/otaswap/otaerror` 驱动进度条+文本显示 |
| CRC16 | CCITT-FALSE 纯 JS 实现（与固件同规格，check=0x29B1） |
| iOS 手势加固 | 全局 `user-select:none` + `-webkit-touch-callout:none`（禁长按选中/拷贝菜单）+ `touch-action:manipulation`（禁双击缩放）；`#joy` 单独 `none`，拖动摇杆不带动页面滚动 |

### 1.1b 台架标定页（calib.html / calib.js，doc/17 M1 + V1.1 §8 + V1.2 §9）

- 入口：控制页维护区链接 `/calib.html`；独立页面、助手自包含（复制 app.js 的 buildFrame/crc16/wsUrl/hello，不共享模块，降低回归面）；
- **V1.2 排布**：顶部 sticky 安全条（状态 + STOP + 配对）+ 四步流程条（①前提 ②判向 ③复核 ④落库，act/done/bad 三态由 `refreshFlow()` 派生）；①列出连接/控制权/车在线/四轮离地四项并显示"还差哪几项"；②判向按钮额外要求车端在线；③把车辆 SVG + 左右"目标·实测" gauge + 4 行 jog 合到一屏，每行 jog 下挂②的结论；④回读时逐轮比对 `0x23.invert` 与②的判定并标出一致/不一致（doc/17 §9）；
- 判向标定：勾选"四轮离地"+二次 confirm 后发**一帧** `0x70`；3s 互锁窗口内抑制驾驶、按钮禁用、进度条走 1.4s；STOP 永远可用；收不到 `"cal"` 时走超时降级文案；status!=0 时未测轮显示"未测"而非完成态的"无计数：查编码器接线"；
- 逐电机点动（§8.1）：4 行 [◀反转][▶正转] 按住即转，30Hz 发 `0x71 {motor,duty=±500}`，松手补发 0；与标定互斥、jog 期驾驶流抑制、固件侧 300ms 超时双保险；页面侧镜像车端门禁——**新鲜遥测**（1s 内）`fault!=0` 时 8 枚按钮禁用、③ 步骤标 bad、提示行改为"车端会拒绝 jog"，遥测缺失或过期不锁死（台架空转场景），门禁跃变在 rAF 里边沿触发刷新；
- 车辆可视化（§8.2）：SVG 俯视图，4 轮 rAF 转角动画（侧级遥测 vMeasL/R，左 A/B 同显）、>1s 无遥测置灰、故障红框、jog 轮高亮；轮位标签按 `0x23` 回传的 pos 动态映射；
- 参数与持久化（§8.3）：hello 成 ctrl 即发 `0x72 REC_GET`，按 `"rec"` 事件渲染位置/编码器方向/fullScale/wheelDia/数据来源；`0x73 REC_SET`（前端范围校验 100..5000 / 30..200）、`0x74 REC_CLEAR`；
- C 侧解码：EVT `0x22`→`{"t":"cal",status,saved,invert[4],delta[4]}`、EVT `0x23`→`{"t":"rec",...}`（bridge.c DIAG 分支按 SF CID 细分拼 JSON；c6_link 映射零改动，0x22/0x23 走既有 DIAG 隧道）；
- 服务端静态路由：`http_server.c` 增 `/calib.html`、`/calib.js` → assets_handler。

### 1.2 资源与回退

- `build_assets.py` 将各文件 gzip（比原文件大则原样存）打包为 `build/assets.bin`
  （16 B 头 + 32 B/项目录 + 对齐载荷），`parttool write_partition --partition-name=assets` 烧写；
- assets 分区为空时，`/` 由 c6_http 回退**内嵌极简页**（决策 C10），保证空分区整机可用。

## 2. 工具链（tools/）

| 脚本 | 职责 | 验证状态 |
|---|---|---|
| `build_assets.py` | assets_src → build/assets.bin（gzip+CRC32+目录） | ✅ 实测（6 文件 22712 B） |
| `sign_bundle.py` | c6.bin(+assets.bin) → 签名 bundle（148 B 头 + ed25519，RFC 8032 纯 Python 签名端） | ✅ 实测（1.05 MB bundle，pubkey 与固件内嵌一致） |
| `ed25519_ref.py` | RFC 8032 参考实现（签名端）；import 自检两条 RFC 向量 | ✅ 自检通过（开发中修正过 point_add/compress 两处公式错误，正反向量兜底） |
| `gen_crypto_consts.py` | 生成 `c6_ota/c6_consts.h`（SHA-512 K/IV、ed25519 d/√-1/L/基点），全整数运算无浮点，IV/K 与已知值断言 | ✅ 已运行入库（重建固件不需要 Python） |

## 3. 密钥口径

- `components/c6_ota/keys/pub_ed25519_dev.bin`（32 B）= EMBED_FILES 进固件的验签公钥；
- `tools/keys/ed25519_dev.seed` = 对应 dev 种子（**台架专用**，hex 明文入库）；
- 量产：换产线密钥对 = 替换上述两文件 + 重新构建固件（LLDD Q2 遗留：若改
  ECDSA P-256 需同步升版 SDD）。

## 4. 验证状态

| 项 | 状态 |
|---|---|
| assets.bin 结构 ⇄ assets_store.c 解析 | ✅ 格式互锁（同源常量），未做解析单测 |
| bundle 生成 ⇄ bundle.c 解析+验签 | ✅ 主机 test_bundle 用真实签名头验证 |
| 页面真机（Portal 弹窗→配对→驾驶→遥测仪表→OTA） | 🔴 G4 |
| logo.svg | 🟢 已提供（顶栏 logo + favicon 共用） |

## 5. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| A-1 | 摇杆驾驶流（30Hz + 心跳复用） | 🟩 | 逻辑完成，真机手感/时延待验 |
| A-2 | 50 Hz 遥测仪表 | 🟩 | 解码实现；帧率依赖 WiFi 实况 |
| A-3 | 配对 UX（含失败提示文案） | 🟩 | |
| A-4 | OTA 上传 + 进度 | 🟩 | otastatus 进度依赖 TC275 STATUS 透传 |
| A-7 | 车速仪表（km/h + 方向 + EMA 平滑 + 陈旧保护） | 🟩 | 逻辑完成并过注入测试；真机数值口径待 TC275 联调 |
| A-8 | 台架标定页 /calib.html（doc/17 M1 + V1.1 §8 + V1.2 §9 四步流程排布） | 🟩 | 逻辑完成、host 单测全绿、mock 注入自验过（0x70 单帧 / 0x71 按住补 0 / 四步状态）；台架验证待做（AC3/9~12 需对串口与真轮） |
| A-5 | 多语言（config.lang） | ⚪ | V1.1 |
| A-6 | 首次开机向导 | ⚪ | V1.1（SDD §11.1） |
