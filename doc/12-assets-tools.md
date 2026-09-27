# 12 控制页与工具链（assets_src / tools）

| 项 | 内容 |
|---|---|
| 代码位置 | `assets_src/`（index.html、style.css、app.js）、`tools/`（build_assets.py、sign_bundle.py、ed25519_ref.py、gen_crypto_consts.py）、`tools/keys/` |
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

### 1.2 资源与回退

- `build_assets.py` 将各文件 gzip（比原文件大则原样存）打包为 `build/assets.bin`
  （16 B 头 + 32 B/项目录 + 对齐载荷），`parttool write_partition --partition-name=assets` 烧写；
- assets 分区为空时，`/` 由 c6_http 回退**内嵌极简页**（决策 C10），保证空分区整机可用。

## 2. 工具链（tools/）

| 脚本 | 职责 | 验证状态 |
|---|---|---|
| `build_assets.py` | assets_src → build/assets.bin（gzip+CRC32+目录） | ✅ 实测（4 文件 9228 B） |
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
| A-5 | 多语言（config.lang） | ⚪ | V1.1 |
| A-6 | 首次开机向导 | ⚪ | V1.1（SDD §11.1） |
