# 09 自身升级与验签（c6_ota）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/c6_ota/`：`ota_self.c`、`bundle.c`、`ed25519v.c`、`sha512.c`、`c6_consts.h`（生成）、`keys/pub_ed25519_dev.bin` |
| 上游需求 | LLDD §4.7（bundle 格式/流程/回滚）、Q2（ed25519）、FR-6、SDD §8（签名） |
| 状态 | 🟡 **92%** — 密码学层主机单测 ✅；OTA 全流程待目标验证；bundle 版本字段缺 |

## 1. 职责

`POST /ota/c6` 的 bundle 流式接收 → ed25519 验签 → 写非活动 app 槽 →
assets 分区更新 → 切启动槽 → 5 s 后自重启；rollback 确认接口。

## 2. 上下文模型（LLDD §4.3"验签挪到 ota_task"）

| 上下文 | 动作 |
|---|---|
| httpd 任务 | `begin/feed/finish`：只把 512 B 块入队（24 深），队满 = 背压（feed 阻塞 ≤2 s） |
| `ota_task`（prio 8 / 6 KB，按需创建） | bundle 解析 + ed25519 + `esp_ota_write` + assets 写 + `set_boot_partition` |

## 3. bundle 格式（tools/sign_bundle.py 生成 ⇄ bundle.c 解析）

```
偏移 0    "C6FW"
      4    u8  fmt_ver = 1
      5    u8  flags    bit0 = has_assets
      6    u16 hdr_len  = 148
      8    u32 total_len
      12   u32 c6_len
      16   32B  c6_sha     = SHA-512(c6.bin)[:32]
      48   u32 assets_len
      52   32B  assets_sha = SHA-512(assets.bin)[:32]
      84   64B  ed25519 签名 ← 覆盖前 84 字节（BUNDLE_SIGNED_LEN）
      148  c6.bin ... assets.bin
```

校验顺序：magic → fmt/hdr_len → 尺寸自洽（total = 148+c6+assets，c6≤3MB，
assets≤512KB，有 assets 必有 flags bit0）→ **公钥非零检查** → ed25519 →
流式 SHA-512 截断比对（任一失败即终止，app 槽 abort 丢弃）。

## 4. ed25519 仅验签（决策 C3）

- 8×32 位肢体域算术（rv32 无 `__uint128_t`；2^256 ≡ 38 mod p 折叠）；
- 点运算：扩展齐次坐标 + **统一加法公式兼做倍点**（RFC 8032 专用 dbl 公式
  在单位元不闭合，会破坏从 identity 出发的 double-and-add——编码期实测修正）；
- 校验流（RFC 8032 §5.1.7）：解压 A（y≥p / 非平方 / x=0 带符号位拒绝）→
  s < L 规范性检查 → h = SHA512(R‖A‖M) mod L → 编码(sB − hA) 与 R 恒时比较；
- 常量（SHA-512 K/IV、d、√-1、L、基点）全部由 `tools/gen_crypto_consts.py`
  用 Python 大数运算生成并断言自检，**零手抄常量**；
- 非常量时间**有意为之**：验签只作用于公开数据。

## 5. 写入编排（ota_task）

```
头解析+验签 OK
  → c6 数据: 惰性 esp_ota_begin(target, total) → esp_ota_write
  → assets 数据: 惰性 erase(4KB 对齐跨度) → esp_partition_write
流终止（DONE/错误/abort）:
  成功: esp_ota_end → esp_ota_set_boot_partition(target) → 回复手机
        → esp_timer 单次 5s → esp_restart（页眉倒计时提示"勿断电"）
  失败: esp_ota_abort → 半槽丢弃（回滚机制兜底）
```

rollback：`ota_self_confirm_rollback(self_check_ok)` —— PENDING_VERIFY 且
自检过 → `mark_app_valid_cancel_rollback`；自检失败 → `mark_app_invalid_rollback_and_reboot`。
由 app_state 45 s 定时器在 LINK 握手成功后调用（01 文档 §4.2）。

## 6. 接口

```c
esp_err_t ota_self_init(void);                     /* app_main 早期调用 */
esp_err_t ota_self_begin(int sd, size_t total);    /* 会话互斥          */
esp_err_t ota_self_feed(int sd, const uint8_t *chunk, size_t n);
esp_err_t ota_self_finish(int sd, char *json, size_t cap);
void      ota_self_abort(int sd);                  /* http close/recv 错误触发 */
int       ota_self_busy(void);
esp_err_t ota_self_confirm_rollback(int self_check_ok);
```

（LLDD §3.3 的 `ota_self_feed` 签名语义一致：`ota_self_state_t` 以
`bundle_state_t` + JSON 结果承载。）

## 7. 资源

| 项 | 值 |
|---|---|
| 上传队列 | 24 × 516 B ≈ 12 KB（begin 时堆分配，finish 释放） |
| ota_task 栈 | 6 KB |
| 验签耗时 | 设计预算 100–300 ms（ota_task 内，不占 httpd） |

## 8. 验证状态

| 层 | 用例 | 结果 |
|---|---|---|
| 主机 | SHA-512 已知向量（abc/空/FIPS 长消息/流式一致性） | ✅ 4/4 |
| 主机 | ed25519 RFC 8032 向量 1/2 + dev 密钥端到端 + 篡改必败 + 坏公钥拒绝 | ✅ 4/4 |
| 主机 | bundle 合法流（逐字节喂入）/坏 magic/坏签名/错公钥/尺寸不符/截断 | ✅ 6/6 |
| 目标 | OTA 全流程 + 断电注入 + 回滚 | 🔴 G4 HIL |

## 9. 完成状态表

| # | 功能 | 状态 | 说明 |
|---|---|---|---|
| O-1 | bundle 流式解析（防越界/防截断） | ✅ | 主机单测 |
| O-2 | ed25519 验签 + SHA-512 | ✅ | 主机单测（RFC 向量） |
| O-3 | A/B 槽写入 + 切槽 + 5s 重启 | 🟩 | 需目标验证 |
| O-4 | assets 分区同步更新 | 🟩 | 同上 |
| O-5 | rollback 确认 | 🟩 | 断电注入待测 |
| O-6 | OTA 中手机断开中止 | 🟩 | 本轮修复（abort 链路） |
| O-7 | bundle 内 fwVer 字段 | 🔴 | 头部无版本字符串（页面经 /api/health 读镜像版本）；防降级版本比对未做 |
| O-8 | anti-rollback 计数（efuse） | ⚪ | `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK_ENABLE=n`，依赖产线 efuse，SDD §8 属产线项 |
| O-9 | 产线密钥替换流程 | 🟡 | 替换 `keys/pub_ed25519_dev.bin` + `tools/keys` 即切换；正式流程/双钥管理未建 |
