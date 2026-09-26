# 01 应用状态机与启动编排（app_main / app_state）

| 项 | 内容 |
|---|---|
| 代码位置 | `main/app_main.c`、`main/app_state.c`、`main/include/app_state.h`、`main/Kconfig.projbuild` |
| 上游需求 | LLDD §4.1（BOOT→…→ONLINE 状态机）、§4.10（自检/堆守护/回滚确认）、§2.3（任务模型） |
| 状态 | 🟡 **85%** — 见文末完成状态表 |

## 1. 职责

1. **组合根**：全系统唯一的跨组件接线点（回调/sink 注册），维护星型依赖规则。
2. **应用状态机**：BOOT → FACTORY_WAIT / NET_START → ONLINE（LLDD 状态图的 C6 侧部分）。
3. **可靠性设施**：10 s 堆守护采样、45 s 回滚确认窗口、诊断聚合（`/api/diag` 数据源）。

## 2. 启动时序（app_main，实现代码顺序）

```
app_main
 ├─ 1 factory_init()          NVS 初始化（失败仅记错误，继续）
 ├─ 2 factory_load()          读出厂资料
 │     └─ 缺失且 CONFIG_C6_FACTORY_DEV_OVERRIDE=y
 │          → 伪造 SN=DEV000 / sddev123456（台架模式，量产必须关）
 │          → have_factory=false
 ├─ 3 ota_self_init()         OTA 互斥锁 + 5s 重启定时器
 ├─ 4 app_state_init()        状态=BOOT、堆基线、10s 堆守护、45s 回滚确认定时器
 ├─ 5 have_factory==false → app_state_enter(APP_FACTORY_WAIT)
 ├─ 6 link_init()             UART1 + rx/tx 任务 + 20ms 健康定时器
 ├─ 7 pair_init() + pair_set_output(bridge_send_frame)
 │    bridge_start()          q_cmd + bridge_task + 20ms 节拍定时器
 ├─ 8 net_start(AP)           成功 → APP_ONLINE；失败 → APP_NET_START
 ├─ 9 http_start()            注册 /ota/c6、/ota/tc275 sink、
 │                            ws_on_binary、session_change→bridge、diag provider
 ├─ 10 net_on_ap_clients(bridge_notify_clients)
 ├─ 11 maint_ble_start()      仅 CONFIG_C6_MAINT_BLE
 │      legacy_tcp_start()    仅 CONFIG_C6_LEGACY_TCP
 └─ 12 app_main 任务进入 60s 周期空转（所有业务在组件任务中）
```

## 3. 状态机（实现态）

```
            ┌──────────────── app_state_init ────────────────┐
            ▼                                                │
          BOOT ──出厂资料缺失且无 DEV_OVERRIDE──► FACTORY_WAIT │
            │ 出厂资料齐全（含台架伪造）                       │
            ▼                                                │
        NET_START ──AP 起来──► ONLINE                         │
            │ AP 起不来                                       │
            └──► NET_START（保持，由用户复位）                 │
```

与 LLDD §4.1 的差异：`SELF_OTA` 与 `REBOOT_PENDING` 两个状态**未进入枚举**——
自身 OTA 的"5 s 后自重启"由 `ota_self.c` 的 `esp_timer` 一次性定时器直接承担，
状态机不感知（缺口见完成状态表 S-3）。

## 4. 关键机制设计

### 4.1 堆守护（10 s 周期）

- 采样 `heap_caps_get_minimum_free_size()`，维护历史最低水位 `heap_min`；
- `< 64 KB` → `ESP_LOGW`（编码计划 §3.5 决策：V1.0 只告警不自动重启，
  重启前诊断通过 `/api/diag` 人工观察）。

### 4.2 回滚确认（45 s 一次性定时器）

- 触发条件：`link_is_up()`（LINK 握手成功 = LLDD 最小自检集成员）；
- 自检集判定：`heap_min ≥ 64KB && self_check==1`（NVS 可读性在 factory_init 已隐式验证）；
- 通过 → `esp_ota_mark_app_valid_cancel_rollback()`；不通过 → 打印并等待下次复位由引导器回滚；
- LINK 45 s 未起来 → 不确认（保持 PENDING_VERIFY，下次复位回滚）。

### 4.3 诊断聚合（app_diag_render → /api/diag）

输出 JSON 字段：`ver / state / slot / factory / reset / selfcheck / coredump /
heap_min / link{up,baud,rtt,crc_err,fmt_err,rx,tx,busy} / pair / uptime_s`。
coredump 存在性经 `esp_core_dump_image_get()` 探测。

## 5. 接口（app_state.h）

```c
app_state_t   app_state_get(void);
const char   *app_state_name(void);
void          app_state_enter(app_state_t st);
void          app_state_init(bool factory_mode);
void          app_state_set_self_check(int ok);
void          app_diag_snapshot(app_diag_t *out);
void          app_diag_render(char *json, size_t cap);
```

## 6. 配置项（main/Kconfig.projbuild）

| 项 | 默认 | 说明 |
|---|---|---|
| `C6_FACTORY_DEV_OVERRIDE` | **y** | 台架伪造出厂资料；量产必须 =n |
| `C6_MAINT_BLE` | n | BLE DPT（还需 CONFIG_BT_ENABLED） |
| `C6_LEGACY_TCP` | n | TCP 8080 直通桥 |

## 7. 资源

| 项 | 值 |
|---|---|
| app_main 栈 | 6144 B（CONFIG_ESP_MAIN_TASK_STACK_SIZE），仅空转 |
| 定时器 | 堆守护 10s 周期 / 回滚确认 45s 一次性（esp_timer 上下文） |
| TWDT | bridge_task 已订阅（5 s）；见缺口 S-2 |

## 8. 完成状态表

| # | 功能 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| S-1 | BOOT 自检 + FACTORY_WAIT 分流 | ✅ | `app_main.c` 步骤 1–5；DEV_OVERRIDE 路径有高亮告警 |
| S-2 | TWDT 初始化与订阅 | 🟡 | bridge_task 已 `esp_task_wdt_add`；**link_rx/link_tx/ota_task 未订阅**（LLDD §2.3 要求 uart_evt_task ✔） |
| S-3 | SELF_OTA / REBOOT_PENDING 状态 | 🟡 | 行为存在（ota_self 5s 重启定时器）但**未纳入状态枚举**，`/api/diag` 的 state 字段在 OTA 期间仍显示 online |
| S-4 | FACTORY_WAIT 仅 DPT 通道开放 | 🟡 | 现实现：进 FACTORY_WAIT 后仍起完整 AP+HTTP（编码计划 C6 台架语义）；产线语义待 DPT 通道定稿后收紧 |
| S-5 | 10s 堆守护 | ✅ | `guard_timer_cb`；只告警（决策记录在案） |
| S-6 | 45s 回滚确认 | 🟩 | `rollback_timer_cb`；逻辑完整，**需断电注入实测**（LLDD G4） |
| S-7 | /api/diag 聚合 | ✅ | `app_diag_render`；coredump 探测已接 |
| S-8 | NVS log_level 应用到日志系统 | 🔴 | factory 存了 `log_level` 但启动时未调用 `esp_log_level_set` |
| S-9 | STA 路线 B 预留 | ⚪ | 凭据字段在 NVS（`net_load_uplink`），激活属 V1.1 |
| S-10 | brownout / panic 策略 / coredump-to-flash | 🟩 | sdkconfig.defaults 全量配置；brownout 阈值默认 |

## 9. 已知限制与风险

1. DEV_OVERRIDE 默认开是为台架点灯；**发版前必须翻 n**（README 与计划文档均已标注）。
2. AP 启动失败后停在 NET_START 无自动重试（LLDD 的"循环重试指数退避"只覆盖 LINK，不覆盖 Wi-Fi）——真机若遇 RF 异常需人工复位。
