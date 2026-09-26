# 15 状态指示灯（c6_led，WS2812）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_led/` |
| 上游需求 | 真机 bring-up 运维需求（无 LLDD 编号） |
| 状态 | 🟩 代码完成，真机验证（COM14，2026-09） |

## 1. 硬件与驱动

- 单颗 WS2812 可寻址 RGB，数据脚 **GPIO8**（ESP32-C6-DevKitC-1 板载位置；
  GPIO8 是 strapping 脚，仅在复位采样，运行期驱动安全）；
- 驱动：RMT TX 通道（`driver/rmt_tx.h`）+ copy encoder，**无外部托管组件依赖**；
  分辨率 10 MHz（100 ns/格），位时序 T0H/T0L/T1H/T1L = 300/800/800/300 ns，
  帧尾 60 µs 低电平（复位码由帧间隔共同满足）；
- 线序 GRB、MSB 先行；颜色预缩到 ≤48/255（约 19% 亮度），夜间不刺眼、
  电流峰小；
- 仅在颜色变化时发送一帧，RMT 总线平时空闲（LED 自锁存）。

## 2. 灯效语义（由 app_state 状态机驱动）

| 模式 | 颜色/节奏 | 含义 | 触发点 |
|---|---|---|---|
| `LED_PAT_BOOT` | 白 500/500 ms 慢闪 | 上电，状态机未动 | `led_init()` 后的默认值 |
| `LED_PAT_FACTORY_WAIT` | 黄 100/100/100/700 双闪 | 等待出厂数据/开通 | `app_state_enter(APP_FACTORY_WAIT)` |
| `LED_PAT_NET_START` | 蓝 250/250 ms 2 Hz | Wi-Fi 启动中 | `app_state_enter(APP_NET_START)` |
| `LED_PAT_ONLINE` | **绿心跳**（100/150/100/1650，2 s 周期双脉冲） | **正常工作** | `app_state_enter(APP_ONLINE)` |
| `LED_PAT_FAULT` | 红 125/125 ms 4 Hz | net start 失败 | `app_main.c` net start 失败分支 |

`app_state.c:app_state_enter()` 是唯一的状态→灯效映射点；
`LED_PAT_OFF`（常灭）保留给生产可关闭场景。

## 3. 配置

| Kconfig | 默认 | 说明 |
|---|---|---|
| `C6_LED_ENABLE` | y | 关闭后 `led_*` 变空实现，调用点无需条件编译 |
| `C6_LED_GPIO` | 8 | WS2812 数据脚 |

## 4. 验证记录

- 真机（COM14）：初始化打印 `c6_led: ws2812 on GPIO8`，无 RMT 错误；
  状态迁移 factory_wait→online 时灯效同步切换，online 后绿心跳稳定；
- IDF v6.1 注意事项：`rmt_tx_channel_config_t.gpio_num`（不是 `gpio`）、
  `rmt_copy_encoder_config_t` 为空结构体（初始化用 `{}`）、
  `rmt_transmit()` 的 config 参数**不可为 NULL**。
