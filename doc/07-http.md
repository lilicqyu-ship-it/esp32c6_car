# 07 Web 服务与会话（c6_http）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_http/http_server.c`、`ws_sessions.c`、`assets_store.c` 及头文件 |
| 上游需求 | LLDD §4.3（Web 服务与会话）、§3.2（端点规格）、FR-2/FR-8 |
| 状态 | 🟡 **92%** — HELLO 缺陷与 OTA abort 缺口已修（待回归联调）；fwVer 缓存 URL 未做 |

## 1. 职责

httpd 生命周期、URI 路由、WebSocket 会话表与遥测广播 pacing、静态 assets
分区流式服务、REST 端点、OTA 上传 sink 分发。

## 2. httpd 配置

| 项 | 值 | 来源 |
|---|---|---|
| max_open_sockets | 8 | LLDD §4.3 |
| 栈 | 8 KB | |
| lru_purge_enable | true | 空闲 socket 回收 |
| WS 支持 | `HTTPD_WS_SUPPORT=y` + PRE/POST handshake CB | sdkconfig.defaults |
| close 回调 | `http_close_cb`：先通知 OTA sink abort（若有在途上传），再清会话 | LLDD §4.6.3 断开即中止 |

## 3. URI 表（注册于 http_start）

| URI | 方法 | 处理 | 鉴权 |
|---|---|---|---|
| `/` `/index.html` `/app.js` `/style.css` `/logo.svg` `/favicon.ico` | GET | assets 分区 gzip 流式（4 KB 块）+ `Content-Encoding: gzip` + `Cache-Control: immutable`；分区空时 `/` 回退内嵌页 | 无 |
| `/ws` | GET(升级) | pre-handshake 校验 token → 角色分配；二进制帧 = proto v2；文本帧 = 控制 JSON | token 可选（无 token = 观赛） |
| `/api/health` | GET | 存活探针 JSON | 无 |
| `/api/diag` | GET | app_state 诊断 JSON（provider 注入） | 无 |
| `/api/pair` | POST | `pair_request`（窗口内）→ 签发 token + 提升角色 | 窗口约束 |
| `/ota/c6` `/ota/tc275` | POST | `?token=` 或 `X-Session-Token` 校验 → 流式 `httpd_req_recv`（512 B 块）→ 注册的 sink（begin/feed/finish/abort） | **控制端 token 必须** |

## 4. WS 会话表（ws_sessions.c）

```c
typedef struct {
    bool used, ws;  int fd;
    ws_role_t role;                 /* NONE / SPECTATOR / CTRL        */
    uint8_t  token_hash[16];        /* SHA-256 截断                    */
    uint32_t last_seq;              /* 防重放第一道：严格单调          */
    uint8_t  hello_sent;            /* HELLO 只发一次                  */
    uint8_t  slow_count;            /* 连续发送失败计数                */
    uint8_t  dead_count;            /* ≥6：仅保活不推遥测              */
    uint32_t tx_frames;
} ws_session_t;   /* 表深 4，互斥保护 */
```

**角色分配**（pre-handshake 回调，握手前完成）：`?token=` 命中
`pair_token_ok` → CTRL（重复带 token 的旧 CTRL 降级为观赛，宽限语义归 pair）；
无 token / 不命中 → SPECTATOR（二进制命令被拒，回 `{"t":"err","e":"auth"}`）。

**命令闸门**（LLDD §4.3，C6 第一道）：`role==CTRL && seq ≠ last_seq` 才放行
入 `q_cmd`（经 `ws_on_binary` 注册的 bridge 回调）；最终裁决在 TC275 E2E。

## 5. 遥测广播 pacing（决策 D1）

`ws_broadcast_binary(f)`（bridge_task 20 ms 调用）→ `ws_sessions_foreach_send`：

```
快照 ws 会话 fd 列表（锁内）→ 锁外逐 fd：
  skip 判定: dead_count>0 → 跳过（仅保活）
             slow_count≥3 → 仅 tick%4==0 发送（12.5 Hz）
  发送: httpd_ws_send_frame_async（异步，不阻塞 bridge）
    成功 → send_ok（清计数）+ LRU 刷新
    失败 → send_fail（slow_count++；≥6 进入仅保活）
```

观测端变慢**不影响控制端**（逐客户端独立 pacing）。文本广播
`http_broadcast_ctl`（配对/OTA 状态推送）同路径，text=true。

## 6. WS 控制面 JSON

| 方向 | 消息 |
|---|---|
| C6→控制端 | `hello{role,ver,tc,pair,ctrl}`（连接建立后一次）、`pong`、`tc{on}`（LINK 上下行）、`baud{v}`、`otastatus{state,pct}`、`otaswap`、`otaerror`、`err{e}` |
| 控制端→C6 | `ping` |

**一切驾驶数据走二进制 proto 帧**（LLDD §3.2），文本面仅会话协商。

控制端两类客户端（2026-09-29 起，见 [00-overview.md](00-overview.md) 控制端行）：
手机控制页全量消费（OTA 系消息仅手机页使用）；S3 遥控器消费 `hello/tc/pong/err`、
忽略其余文本消息，自定时 `ping` 1 Hz 测 RTT、10 s 无任何下行即整链重连——
均对齐手机页 app.js 口径。

## 7. assets_store（assets 分区）

```
assets.bin: "ASSETS"|ver=1|rsv|count u16|total u32|pad2
            entry[count]{name[16],off u32,gz_len u32,raw_len u32,crc32 u32}
            gzip 载荷（4 字节对齐）
```

`assets_find(path)` 精确名字匹配 → `assets_read` 4 KB 块流式发送；
分区缺失/空 → `/` 回退内嵌 HTML（决策 C10），`/api/diag` 侧可感知。

## 8. 验证状态

编译级验证（G2）。本轮文档梳理发现并已修复两处缺陷（修复后重新编译通过）：
1. HELLO 在 pre-handshake 启用时永不发送（`hello_sent` 标志修复）；
2. 手机断开时 OTA 上传不立即中止（sink 增加 `abort()`，close/recv 错误路径均触发）。

## 9. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| H-1 | URI 表 + REST 端点 | ✅ | |
| H-2 | WS 会话表 + 角色分配 + SEQ 闸门 | ✅ | |
| H-3 | 遥测 pacing（慢客户端降频/仅保活） | ✅ | 逻辑完整，多客户端实测待 HIL |
| H-4 | HELLO 首屏推送 | 🟩 | 缺陷已修，待真机回归 |
| H-5 | assets 分区流式服务 | 🟩 | 格式与解析器就绪；需真机抓包 |
| H-6 | 内嵌回退页 | ✅ | |
| H-7 | OTA 上传 sink 分发 + abort | 🟩 | abort 缺口已修 |
| H-8 | assets URL 带 fwVer 防陈旧缓存 | 🔴 | 已有 `Cache-Control: immutable`，URL 版本化未做（页面通过 /api/health 获取版本替代） |
| H-9 | 会话表主机单测 | 🔴 | ws_sessions 依赖 FreeRTOS 互斥，未做主机测（LLDD §9 列项） |
| H-11 | **真机首测缺陷修复**：启动时序竞态 | ✅ | bridge_task 先于 httpd 启动即查询客户端集合 → 对 NULL 互斥句柄 assert panic（`xQueueSemaphoreTake` 死循环重启，真机 2026-09-26 首烧复现）。修复：ws_sessions 全部访问器对未初始化状态容错返回安全值；同类隐患一并收敛（bridge_post_cmd/link_send 的队列未创建守卫） |
| H-10 | 慢客户端降级观测指标暴露 | 🔴 | slow/dead 计数未进 /api/diag |
