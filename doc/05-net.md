# 05 接入网（c6_net）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_net/net.c`、`captive_dns.c`、`mdns_lite.c` + 头文件 |
| 上游需求 | LLDD §4.2（接入网设计）、FR-1 |
| 状态 | 🟡 **90%** — softAP/DNS/mDNS 实现；Portal HTTP 302 属 httpd 侧尚未做、真机未验 |

## 1. 职责

softAP 生命周期、Captive Portal 的 DNS 野答、`mycar.local` mDNS 解析、
Wi-Fi 事件（站点增减）向 bridge 的通知、路线 B STA 凭据预留。

## 2. softAP（net.c）

| 项 | 值 | 说明 |
|---|---|---|
| SSID | `SD-` + SN 末 6 位 | `factory_ap_ssid()` |
| 密码 | NVS `wifi_pass`（WPA2-PSK） | 出厂 12+ 位随机，标签印刷 |
| 信道 | NVS `ch`，默认 6 | 仅允许 1/6/11 |
| max_connection | 4 | 1 控制 + 2 观赛 + 1 产测余量 |
| 模式 | WIFI_MODE_AP | APSTA 为路线 B 预留（未启用） |
| AP 地址 | 192.168.4.1（静态默认） | IDF 6.x 已移除 `IP_EVENT_AP_GOT_IP`，直接用默认值 |

事件处理（`net_event_handler`）：`AP_START/AP_STOP/AP_STACONNECTED/AP_STADISCONNECTED`
→ 只维护计数与回调投递，**不做业务**（IDF 事件任务约束，LLDD §4.2）。
站点增减 → `net_clients_cb_t` → bridge → 0x42 LINK_STATE。

## 3. Captive DNS（captive_dns.c）

- UDP :53，wildcard 应答：任何 A 查询 → `A 192.168.4.1`（TTL 60）；
- 应答构造：复制请求头+问题段，置 `QR=1 AA=1`，按 qdcount 逐条追加
  压缩指针（0xC00C）型答案；
- 任务 `captive_dns`（prio 4，3 KB 栈），阻塞 recvfrom 循环。

## 4. mdns_lite（自实现，决策 C2）

IDF v6.x 内置 mdns 组件移入组件仓库（需构建期联网拉取），为消除该依赖自实现 ~250 行响应器：

| 查询 | 应答 |
|---|---|
| A `mycar.local` | A 记录 → AP IP（TTL 120） |
| PTR `_http._tcp.local` | → `<SSID>._http._tcp.local`（TTL 4500）+ 附加 SRV(port 80)/A/TXT(`path=/`) |

实现要点：UDP :5353 + `IP_ADD_MEMBERSHIP(224.0.0.251)`；名称比较大小写不敏感；
不响应压缩指针型查询（查询方为手机解析器，通常不会压缩问题段）。

## 5. 接口

```c
esp_err_t net_start(const net_cfg_t *cfg);        /* AP + captive DNS + mDNS */
void      net_on_ap_clients(net_clients_cb_t cb); /* 站点增减 → LINK_STATE   */
const char *net_ip_str(void);                     /* "192.168.4.1"           */
int       net_sta_count(void);
esp_err_t net_load_uplink(char *ssid, size_t, char *pass, size_t);  /* 路线 B 预留 */
esp_err_t captive_dns_start(const char *ap_ip);
void      captive_dns_set_ip(const char *ip);
esp_err_t mdns_lite_start(const char *hostname, const char *instance, const char *ip);
```

## 6. 资源

| 任务 | 栈 | 优先级 |
|---|---|---|
| captive_dns | 3 KB | 4 |
| mdns_lite | 3.5 KB | 4 |

## 7. 验证状态

编译级验证（G2）。DNS 报文构造与 mDNS 名称匹配为纯逻辑，但未做主机单测
（lwip socket 依赖）；iOS/Android Portal 弹窗触发需要真机验证。

## 8. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| N-1 | softAP（SSID/密码/信道/max_conn） | 🟩 | 待真机确认扫描/连接 |
| N-2 | Captive DNS 野答 | 🟩 | 报文格式经构造；未抓包验证 |
| N-3 | Captive Portal HTTP 302 | 🔴 | LLDD 要求"非本机 Host 头 302 → /"；当前只有 DNS 野答，无 HTTP 层重定向 handler（iOS/Android 部分场景仅靠 DNS 也能弹窗） |
| N-4 | mDNS（A + _http._tcp） | 🟩 | 简化实现；`mycar.local` 解析待真机 |
| N-5 | 站点计数 → LINK_STATE | ✅ | 回调链已接 |
| N-6 | STA 路线 B | ⚪ | 凭据 NVS 读写就绪，模式切换/OTA 拉取 V1.1 |
| N-7 | Wi-Fi 事件只投递不做业务 | ✅ | 设计约束落实 |
