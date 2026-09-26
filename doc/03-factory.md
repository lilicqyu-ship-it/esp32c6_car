# 03 出厂数据（c6_factory）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_factory/factory.c`、`include/factory.h` |
| 上游需求 | LLDD §4.8（NVS 键表与写入者）、§4.4（会话宽限持久化） |
| 状态 | 🟡 **90%** — 读写 API 全量实现；产线写入入口（DPT）未接线 |

## 1. 职责

加密 NVS 命名空间 `c6fact` 的**唯一读写网关**：SN、Wi-Fi 密码、配对盐、RF 通道、
日志级别、路线 B 上行凭据（预留）、预配对 token 表（产线批量预配对）、
会话宽限哈希。其他组件一律不得直接触碰该命名空间。

## 2. 键表（实现态，LLDD §4.8 全量）

| 键 | 类型 | 写入者 | 读取者 |
|---|---|---|---|
| `sn` | str(≤16) | 产线 DPT（入口待接） | net（SSID 派生）、app_main |
| `wifi_pass` | str(≥8,<64) | 产线 DPT | net（AP 密码） |
| `pair_salt` | blob(16) | 产线 DPT | 预留（token 派生扩展） |
| `ch` | u8 (1/6/11) | 运维 | net |
| `log_level` | u8 | 运维 | app_state（**应用缺失**，见 01 文档 S-8） |
| `uplink_ssid` / `uplink_pass` | str | 用户 | `net_load_uplink()`（路线 B 预留） |
| `ppt0..ppt3` | blob(16)×4 | 产线 DPT | 预配对免按键（**读取路径未接**） |
| `sess_tok` / `sess_exp` | blob(16) / i64 | c6_pair | c6_pair（30s 宽限） |

安全属性：NVS 分区加密（`CONFIG_NVS_ENCRYPTION=y` + `nvs_keys` 分区，
flash 密钥方案）；token **只存 SHA-256 前 16 B 哈希**，永不落明文。

## 3. 接口（factory.h 全量）

```c
esp_err_t factory_init(void);                        /* nvs_flash_init 容错 */
esp_err_t factory_load(factory_data_t *out);         /* 快照，缺省填默认值 ch=6 */
void      factory_ap_ssid(const factory_data_t *f, char *out, size_t cap);
          /* "SD-" + SN 末 6 位；SN 短于 6 位直接前置，无 SN → "SD-" */

esp_err_t factory_write_sn(const char *sn);          /* 1..16 字符校验 */
esp_err_t factory_write_wifi_pass(const char *pass); /* WPA2 ≥8 字符校验 */
esp_err_t factory_write_channel(uint8_t ch);         /* 仅 1/6/11 */
esp_err_t factory_write_log_level(uint8_t level);
esp_err_t factory_write_ppt(uint8_t slot, const uint8_t hash[16]);

esp_err_t factory_save_session(const uint8_t hash[16], int64_t expiry_us);
esp_err_t factory_load_session(uint8_t hash[16], int64_t *expiry_us);
esp_err_t factory_clear_session(void);
```

`factory_load` 在命名空间不存在时返回错误码（app_main 据此判 FACTORY_WAIT），
`factory_data_t` 恒为完整快照（`have_sn/have_pass` 标志位区分缺省）。

## 4. 数据结构

```c
typedef struct {
    char    sn[17];  char wifi_pass[64];
    uint8_t salt[16];  uint8_t channel;  uint8_t log_level;
    char    uplink_ssid[33];  char uplink_pass[64];
    uint8_t ppt[4][16];
    bool    have_sn, have_pass;
} factory_data_t;
```

## 5. 时序

- **启动**：`factory_init → factory_load` → 无 SN/pass 且 DEV_OVERRIDE=n → FACTORY_WAIT（01 文档）。
- **配对成功**（c6_pair 调用）：`factory_save_session(hash, now+30s)` —— 写入即宽限生效；
  C6 重启后 `pair_init → factory_load_session` 恢复宽限（过期即清除）。

## 6. 错误处理

- 全部 API 返回 `esp_err_t`；参数非法（SN 长度、密码长度、通道值、slot 越界）回 `ESP_ERR_INVALID_ARG`；
- NVS commit 失败向上传递，调用方记录日志（会话宽限失败仅 WARN，不阻断配对）。

## 7. 验证状态

编译级验证（G2）；键读写逻辑依赖 NVS 加密分区，主机端无法单测（需目标机）。
`pair` 相关的会话保存/恢复由 06 模块联调覆盖。

## 8. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| F-1 | 键表读写 API 全量 | ✅ | 含参数校验 |
| F-2 | 会话宽限持久化/恢复/清除 | ✅ | 06-pair 消费 |
| F-3 | AP SSID 派生（SN 末 6 位） | ✅ | `factory_ap_ssid` |
| F-4 | 产线 DPT 写入入口 | 🔴 | API 就绪；BLE DPT C6 本地项（10 文档 M-3）未实现，当前唯一写入口是台架 DEV_OVERRIDE |
| F-5 | 预配对表（ppt0–3）消费 | 🔴 | 写入 API 就绪；WS 升级时比对 ppt 表的逻辑未接（用户开箱免按键路径） |
| F-6 | pair_salt 参与 token 派生 | ⚪ | V1.0 token 直接随机 32B，盐位预留 |
