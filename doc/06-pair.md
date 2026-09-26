# 06 配对与会话（c6_pair）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_pair/pair.c`、`include/pair.h` |
| 上游需求 | LLDD §4.4（职责切分与时序）、§8.2（SDD 配对流程）、FR-4 |
| 状态 | 🟩 **100%**（代码级）— 待端到端联调 |

## 1. 职责切分（决策 D3）

- **TC275 = 授权权威**：车侧按键开窗、窗口计时、PAIR_CONFIRM 裁决；
- **C6 = 会话执行者**：socket↔角色绑定、token 签发、30 s 断线宽限。

## 2. 状态机

```
              PAIR_NOTIFY{window_s}（LINK 0x51, TC275 发）
   IDLE ────────────────────────────────► OPEN (window_until = now + window_s)
     ▲                                      │
     │ 窗口过期 / 配对请求被消费（无论成败）  │ POST /api/pair
     │◄─────────────────────────────────────┤
     │         PAIR_REQ{sd} → TC275 ─── CONFIRM{ok}（≤3s 等待）
     │                                      │
     │ ctrl_gone()（控制端 WS 断开）         ▼
     │◄─────────────────────────────── CLAIMED（token 已签发）
     │
     └── token 重连：WS 升级带有效 token（live 哈希或未过期宽限哈希）→ 直接 CLAIMED
```

注意：`OPEN` 是**一次性窗口**——一次 pair_request 消费后无论成败都回 IDLE。

## 3. 配对时序（与 LLDD §4.4 一致）

```
手机                    C6(c6_pair)                     TC275
 │ WS 升级(无 token)      │                              │
 │◄─ hello{role:spectator}│                              │
 │                  用户按车键 3s                         │
 │                        │◄─ 0x51 NOTIFY{window=60s} ───│
 │ POST /api/pair         │                              │
 │──────►│ 锁定状态检查    │                              │
 │        │ 0x51 REQ{sd} ────────────────────────────────►│ 窗口内 → CONFIRM
 │◄─ 200{role:ctrl,token}│ 签发 32B 随机 token（hex64）    │
 │        │ sha256[:16] 存 NVS{hash, now+30s}             │
 │        │ ws_sess_promote(sd, hash) → LINK_STATE=2      │
```

拒绝路径：窗口外 → TC275 REJECT → HTTP 403 `no window`；已 CLAIMED → 409 busy；
3 s 无 CONFIRM → 504 timeout；LINK 不可用 → 503。

## 4. token 与宽限设计

| 项 | 设计 | 落点 |
|---|---|---|
| token 形态 | 32 B `esp_fill_random` → 64 字符 hex 给手机 | `pair_request` |
| 存储 | SHA-256 截断 16 B：内存 live + NVS（`sess_tok`,`sess_exp=now+30s`） | `factory_save_session` |
| 重连（免按键） | WS 升级 `?token=` → 哈希比对 live → 宽限（NVS 恢复的 grace 也比对，命中即提升 CTRL） | `pair_token_ok` |
| 断线语义 | 控制端 WS 断开 → `pair_ctrl_gone()` 状态回 IDLE + **立即** LINK_STATE=0（不等 30 s）；30 s 只影响"免按键重连"，停车语义归 TC275 心跳——二者解耦（LLDD §4.4） | `pair_ctrl_gone` |

## 5. 接口（pair.h 全量，与 LLDD §3.3 一致）

```c
esp_err_t    pair_init(void);                    /* 恢复未过期宽限 */
void         pair_set_output(pair_output_fn out);/* bridge 注入 link_send 路径 */
pair_state_t pair_state(void);                   /* IDLE / OPEN / CLAIMED */
void         pair_get_session(pair_session_t *out);
void         pair_on_frame(const proto_frame_t *f);   /* bridge 路由 PAIR 帧 */
pair_result_t pair_request(int sd, char *token_hex, size_t cap); /* httpd 上下文，阻塞≤3s */
void         pair_ctrl_gone(void);
bool         pair_token_ok(const char *token_hex);
void         pair_hash_token(const char *token_hex, uint8_t out[16]);
```

`pair_output_fn` 注入遵守"LINK TX 唯一编排"约束：pair 只组帧，经 bridge 注册的
`bridge_send_frame → link_send` 出线。

## 6. 线程模型

`pair_request` 运行在 httpd 任务（阻塞等 CONFIRM 信号量 ≤3 s）；`pair_on_frame`
运行在 bridge_task；共享状态由 `mtx` 保护；`confirm` 二值信号量做跨任务握手。

## 7. 验证状态

编译级验证（G2）。窗口/宽限/重连时序属 IDF pytest-embedded 用例集（LLDD §9
目标单测层），未编写；HIL 联调覆盖。

## 8. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| PR-1 | 窗口跟随（NOTIFY→OPEN→消费/过期） | ✅ | 一次性窗口语义 |
| PR-2 | PAIR_REQ 发起 + CONFIRM/REJECT/超时三分支 | ✅ | 3 s 等待 |
| PR-3 | token 签发（32B 随机，仅存哈希） | ✅ | |
| PR-4 | 30 s 宽限（NVS 持久 + 重启恢复） | ✅ | `pair_init` 恢复逻辑 |
| PR-5 | token 重连免按键（宽限哈希比对即提升） | ✅ | `pair_token_ok` |
| PR-6 | 旧 CTRL 宽限拒绝规则（LLDD：旧 CTRL 断开<30s 时新 PAIR_REQ 拒绝） | ✅ | CLAIMED 状态即 BUSY；断开后 IDLE+宽限仅服务 token 重连 |
| PR-7 | 预配对表免按键（ppt0–3 比对） | 🔴 | API 在 c6_factory，未接入 `pair_token_ok` |
