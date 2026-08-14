# P3-2 认证体系三协议共存 —— 实施规格

> 目标内核：PostgreSQL 18.3 融合内核（PG 5432 / MySQL 3306 / TDS 1433 同实例并行）
> 仓库根：`/home/hlv/openhalo-update/postgresql_modified_for_babelfish`
> 文档性质：**规格文档，不含代码改动**
> 标注约定：【核实 `文件:行号`】= 已读源码确认；【推断】= 基于已核实事实的设计推理，未经实测

## ⚠️ 2026-08-14 上游比对与事实核查结论（先读这段）

**关于"要不要做"**：P3-2 与 P2-8/P2-15 性质不同，**不适用"不超越上游"的否决原则**。openHalo 只实现单一 MySQL 兼容模式（`initdb -m` 二选一），Babelfish 只融合 PG+TDS 两种协议，两者都从未面对"同一账户体系要横跨三种协议线路认证"这个场景——这是 FUSION_PLAN.md 里明确标注的"本轮发现"，是三协议并行架构本身带来的结构性新问题，不是某个方言兼容特性的缺失。三阶段执行顺序里"先走通三协议并行"这一步，账户认证层面其实还没有真正走通，所以 P3-2 应该做。

**发现一处影响方案设计的重大事实错误，已更正**：

本文档 §1.1（原第 32 行）声称"`babelfishpg_tds` 在本仓库中只有编译产物……没有源码"，并据此把 TDS 认证整体划入"不在范围内"（§10.1）与"更大改造"（G4）。**这是错误的**——`babelfishpg_tds` 的完整源码就在 `/home/hlv/openhalo-update/babelfish_extensions/contrib/babelfishpg_tds/`（`BABEL_6_0_STABLE` 分支），本仓库的路径判断方法本身有误（大概率是只搜了 `postgresql_modified_for_babelfish` 这一个仓库）。

更重要的是，读了真实源码后发现一个**简化整个方案**的事实：`contrib/babelfishpg_tds/src/backend/tds/tdslogin.c` 的 `CheckAuthPassword()`（约第 1281-1310 行）直接调用**内核原生** `get_role_password()` 取密码，再用**内核原生** `plain_crypt_verify()` 校验——TDS **完全复用 PG 的 `rolpassword`（SCRAM/MD5）存储格式，不存在任何 TDS 专属 verifier 格式**。也就是说：

- 真正的"密码格式互斥"只发生在 **MySQL 一侧**（`mysql_native_password` 的无盐单轮 SHA1 与 PG 的 SCRAM/MD5 不兼容），TDS 与 PG 之间根本没有格式冲突。
- §2.2 表格里"`COMPAT_PROTOCOL_TDS` | 同 MySQL 规则，`wanted` 换成 TDS 的 verifier 类型"这一行应改为：**`COMPAT_PROTOCOL_TDS` 与 `COMPAT_PROTOCOL_POSTGRES` 走同一分支**（只读 `rolpassword`），`get_role_password_ext(kind=TDS, ...)` 不需要任何新格式、不需要触碰 `rolpasswordext`。
- §10.1 的 G4（"TDS 侧认证……需要先取得源码"）应删除：源码已具备，且不需要为 TDS 设计任何新东西，`kind=TDS` 分支是免费的。
- 连带地，§8.3 的 SEC-1（"`protocol_kind == POSTGRES` 的连接永不读 `rolpasswordext`"）应扩展为 **`protocol_kind ∈ {POSTGRES, TDS}` 的连接永不读 `rolpasswordext`**。

**其余核心论据经抽查，均属实**（已用 Read/grep 逐一核实源码）：`get_role_password()` 无协议感知（`crypt.c:44-91`，仅查 `pg_authid.rolpassword` + 校验 `rolvaliduntil`）、`pg_authid` 确实只有单列 `rolpassword` 且不在 `toasting.h` 的 TOAST 列表中、`check_hba()` 确实零协议判定（`hba.c:2544` 起只有 conntype/SSL/GSS/IP/database/role 六个维度）。D4（MySQL 认证失败路径不调用 `ClientAuthentication_hook`）此前已单独验证与 openHalo 上游完全一致。

**根因溯源补充**：openHalo 源里的 `src/backend/adapter/mysql/mysql_auth.c`（融合树对应 `contrib/aux_mysql/src/mysql_auth.c`，路径变化是融合架构调整，非 bug）同样调用无协议感知的原生 `get_role_password()`，同样只在 HBA 白名单里接受 `uaMD5`。也就是说"单密码槽位、格式互斥"这个根因**本来就在 openHalo 里**，只是从未暴露成真实冲突——因为 openHalo 是 `initdb -m mysql` 单模式，从不需要同一账户体系再服务 PG 协议连接。这印证了前面的定性：P3-2 是三协议融合产生的新场景，根因虽然继承自 openHalo 的既有设计，但"要不要解决"这件事本身不适用"上游没做就不做"的否决逻辑，因为 openHalo 从未有机会面对这个场景。

**下一步建议**：本文档 §2~§9 的方案设计（`rolpasswordext` 新列、`protocol_mask` HBA 字段等）基本仍然成立，但因为 TDS 侧的范围判断变了（从"需要额外设计"变成"零成本复用"），实施前应该先用上述更正过一遍 §2.2、§6（变更清单）、§7（G4 应删除或改写为"确认 kind=TDS 分支复用 PG 逻辑，补充测试用例即可"）、§8.3（SEC-1）、§10（不再需要把 TDS 列为"不在范围内"）。这是设计层面的订正，工作量不大，但不建议跳过就直接照单实施。

---

## ✅ 2026-08-14 实施结果：M1-M7 全部完成并实测验证（含 M5 撤回部分的收口）

**实施顺序按 §6.3 建议的 `M6 → M7 → M5 → …`**；M1-M4 在用户确认 initdb 影响后也已实施完毕（见下方「M1-M4 实施结果」）。M5 撤回的视图可观测性部分由收口提交 `f9b664c2b1` 补上，且该提交同时修正了 `992100e923` 漏做的 catversion bump（详见 M5 一节末尾）。

### M6（修 D4）—— 与规格建议有出入，已用 PG 原生源码逐行核实并修正

逐行读了 `src/backend/libpq/auth.c` 的 `ClientAuthentication()`（379-670 行）后发现：**PG 自己的 `uaReject`/`uaImplicitReject` case 是直接 `ereport(FATAL)` 跳出，根本不设置 `status`、不会执行到 663 行的 hook 调用**——只有真正进入某个认证方法判定（`CheckPWChallengeAuth` 等）返回失败，才会走到 hook。

这意味着规格 §6.1 建议的 4 处（`mysql_auth.c` 698/705/786/802）里，**698（HBA reject）和 705（HBA 方法不支持）不应该加**——那正好对应 PG 自己在 HBA 判定阶段也不调用 hook 的行为，加了反而是让 MySQL 侧比 PG 原生多做。实际只在 **786 行**（取不到密码）和 **802 行**（密码比对失败）两处插入了：
```c
if (ClientAuthentication_hook)
    (*ClientAuthentication_hook) (port, STATUS_ERROR);
```

**端到端验证**（加载 `auth_delay` 扩展，`auth_delay.milliseconds = 3000`）：
| 场景 | 触发路径 | 实测耗时 |
|---|---|---|
| 密码错误 | 802 行 | 3.007s（延迟生效） |
| 用户不存在 | 786 行 | 3.009s（延迟生效） |
| HBA `reject` | 698 行（未加 hook，对照组） | 0.007s（符合"不该加"的判断） |

三组耗时精确验证了修复位置的选择是对的，不是拍脑袋决定的。

### M7（修 D1 注释）—— 已完成

`crypt.h` 的注释被修正为准确描述现状：`password_encryption` 枚举确实包含 `mysql_native_password`（`guc_tables.c:418-422` 已核实），但 PG 原生认证路径（`plain_crypt_verify`/`CheckPWChallengeAuth`）拒绝这个格式。

### M5（HBA protocol_mask）—— 核心过滤逻辑已完成并实测验证；视图展示列已撤回，后由 `f9b664c2b1` 收口补上

**范围调整**：规格 §4.3 改动面清单里的 `hbafuncs.c`（`fill_hba_line()` 输出新列）和 `pg_proc.dat`（`pg_hba_file_rules` 返回列 +1）**已经实施后又撤回**——因为改 `pg_proc.dat` 会改变 `pg_hba_file_rules()` 的函数签名，这是 `initdb` 时写入 `pg_proc` 系统表的静态数据，运行时不会跟着代码同步，必须 `initdb` 才能生效。这与"M5 不需要 initdb"的前提矛盾，所以保留核心过滤机制、撤回视图可观测性这部分，留给 M1 一起做一次 initdb。

**✅ 收口（2026-08-14，`f9b664c2b1`）**：M1 的 initdb 轮次没有带上这部分，故在收口提交里补完——`pg_hba_file_rules()` 新增 `protocol` text 输出列（`hbafuncs.c` 的 `NUM_PG_HBA_FILE_RULES_ATTS` 11→12 + `protocol_mask_to_str()` 渲染：默认全协议掩码渲染为 `postgres,mysql,tds`，解析错误行渲染 NULL；`pg_proc.dat` 的 `proallargtypes`/`proargmodes`/`proargnames` 同步 +1），client-auth.sgml 补 `protocol=` 选项文档，`rules.out`/`004_file_inclusion.pl` 跟随新列。**同一提交补上了 `992100e923` 漏改的 catversion bump（`202506291`→`202506292`）**——加 `rolpasswordext` 列却不动 catversion 会让旧数据目录启动时得不到任何保护。实测：四套件 13/13；全新三协议集群上 `protocol=` 强制力、双 verifier 登录、TDS md5 登录均通过。

**已完成、且不需要 initdb 的部分**：
- `hba.h`：`HbaLine` 加 `protocol_mask` 字段 + `HBA_PROTOCOL_MASK_ALL` 默认值宏（用全 1 而非 `(1u << COMPAT_PROTOCOL_KIND_MAX) - 1`，因为 `hba.h` 不能 include `libpq-be.h`，避免循环依赖，全 1 的语义在协议种类增加后依然正确）
- `hba.c`：`parse_hba_line()` 里 `palloc0()` 后设默认值、`parse_hba_auth_opt()` 新增 `protocol=` 选项解析分支、`check_hba()` 循环体最前面加匹配判定

**发现并修正了规格 §4.2 示例本身的一处 bug**：`protocol=postgres,tds` 这种裸写法会被 HBA 的 tokenizer 按逗号拆成两个 token（`protocol=postgres` 和裸 token `tds`），导致 `authentication option not in name=value format: tds` 解析错误。正确写法必须像 `radiusservers` 那样加引号：`protocol="postgres,tds"`。已用这个修正过的语法做了三协议交叉验证。

**端到端验证**（`/tmp/w8testdata`，测试完已恢复原配置）：
```
host  all  all  127.0.0.1/32  md5      protocol="postgres,tds"
host  all  all  127.0.0.1/32  reject   protocol=mysql
```
| 协议 | 端口 | 结果 |
|---|---|---|
| PG (`psql`) | 5432 | 匹配到 `protocol="postgres,tds"` 行，md5 登录成功 |
| TDS (`tsql`) | 1433 | 同一行同样生效，登录成功且 `SELECT 1` 正常返回 |
| MySQL (`mysql`) | 13306 | 匹配到独立的 `protocol=mysql reject` 行，被拒绝，且没有误落到前一条更宽松的规则 |

**顺带验证了一个规格里没写清楚的事实**：TDS 认证（`babelfishpg_tds` 的 `tdslogin.c:1687`）确实调用了 `hba_getauthmethod()`，走的是同一套 `check_hba()`，所以 `protocol_mask` 对 TDS 是真正生效的，不是空中楼阁的假设。同时发现 TDS 不支持 `scram-sha-256`（`tdslogin.c` 里 `uaSCRAM` 落在"unsupported"分支），只支持 `md5`/`password`/`trust`/`gss`/`oauth`——测试时踩了这个坑，换成 `md5` 才通过。

### ✅ 2026-08-14 M1-M4 实施结果：全部完成并通过 A1-A13 验收用例

用户同意后执行了 initdb（旧数据目录备份为 `/tmp/w8testdata.pre_rolpasswordext_bak`，新目录仍是 `/tmp/w8testdata`）。

**M1（`pg_authid` 加 `rolpasswordext` 列）**：完成，但规格 §3.2 S3 方案里的一处"推断"被证伪——原文说"`pg_authid.dat` 无需改动：缺失的尾部可空列由 genbki 自动补 `_null_`"，实测编译报错 `missing values for field(s) rolpasswordext in pg_authid.dat line 26`。genbki 不会自动补全，`.dat` 里的每条记录（17 条）都必须显式加上 `rolpasswordext => '_null_'`，已手动补全。

**M2（`get_role_password_ext()`）**：完成，实现比规格 §2.2 的表格更精确一处——按早些时候的上游比对更正，`kind == COMPAT_PROTOCOL_TDS` 和 `COMPAT_PROTOCOL_POSTGRES` 走同一分支（只读 `rolpassword`，直接调用 `get_role_password()`），不是"同 MySQL 规则换 verifier 类型"。只有 `COMPAT_PROTOCOL_MYSQL` 才走 `rolpasswordext` 优先、`rolpassword`（且类型匹配）回落的双源查找。另外实现了 `find_verifier_of_type()` 辅助函数落实 IV-1/IV-2/IV-3：整个 verifier 列表只要有一条是 MD5/SCRAM/无法识别的格式，就整体判定为损坏、fail-closed 拒绝，不做部分信任。

**M3（`user.c` 写入分流）**：完成。CREATE ROLE 和 ALTER ROLE 都按 `get_password_type(shadow_pass)`（加密后的实际类型）分流，不看 `password_encryption` GUC 本身，因为 `encrypt_password()` 有直通路径会让两者不一致（已用 A12 验证）。ALTER ROLE 侧新增了 `merge_verifier_into_list()`（放在 `crypt.c`，因为职责上属于"verifier 列表编解码"而非 DDL 语义）处理 `rolpasswordext` 的同类型覆盖、其余类型保留（IV-1）；读旧值时最初写成 `SysCacheGetAttr(AUTHNAME, tuple, ...)`，但 `get_rolespec_tuple()` 在 `CURRENT_USER`/`CURRENT_ROLE` 场景下返回的其实是 `AUTHOID` 缓存的 tuple，硬编码 cache id 不安全，改成了文件里 `RenameRole` 已经在用的 `heap_getattr()` 惯用法。`PASSWORD NULL` 改为同时清空两列（SEC-5）。

**M4（`mysql_auth.c` 接线）**：完成，`get_role_password(port->user_name, &logdetail)` 改为 `get_role_password_ext(port->user_name, COMPAT_PROTOCOL_MYSQL, PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD, &logdetail)`。

**A1-A13 验收结果**（`/tmp/w8testdata`，测试角色已清理）：

| 用例 | 结果 |
|---|---|
| A1 先 scram 后 mysql，两协议登录 | 通过 |
| A2 顺序颠倒 | 通过 |
| A3 只设 scram，3306 拒绝 | 通过 |
| A4 只设 mysql，5432 拒绝 | 通过（第一次测试因 HBA 误配成 `trust` 而失败，改回真正需要口令的方法后通过——测试环境问题不是代码问题） |
| A5 `PASSWORD NULL` 全清 | 通过，两列都为 NULL 且两协议都无法登录 |
| A6 `DROP ROLE` + 重建同名 | 通过，旧口令失效（S3 存储方案天然满足） |
| A9 `trust` 不会被 MySQL 拿到 | 通过（HBA 分流本身即保证） |
| A11 `rolpasswordext` 手工改成畸形串 | 通过，fail-closed 拒绝，日志有 `LOG: rolpasswordext contains an invalid verifier entry` |
| A12 直通路径按实际类型分流 | 通过，GUC 仍是 scram 时粘贴 mysql verifier，正确落 `rolpasswordext` 不污染 `rolpassword` |
| A13 `pg_shadow`/`pg_roles` 不暴露新列 | 通过 |
| TDS 认证路径回归 | 通过（认证本身成功，卡在"tsqldb 数据库不存在"——测试环境没跑 babelfish 初始化脚本，不属于本次改动范围） |
| A7/A8/A10 | 分别在 M5 阶段及 D4 端到端测试中已验证，逻辑未变 |

**验收标准（§6 目标）已达成**：同一个角色，同一个明文口令，能同时通过 5432 的 scram-sha-256 和 3306 的 mysql_native_password 登录；PG 原生认证行为、`pg_hba.conf` 语义、既有工具零回归。

---

---

## 0. 结论摘要

| 问题 | 结论 |
|---|---|
| `get_role_password()` 能否按协议分派 | **能，且改动很小**。函数签名里根本没有 `Port`/协议参数，是一个纯粹的"角色名 → rolpassword"查询【核实 `src/backend/libpq/crypt.c:44-91`】。只需新增一个带 `CompatibilityProtocolKind` + 期望 `PasswordType` 的重载版本，把现有函数保留为 PG 语义薄封装即可。 |
| 一个角色能否持有多份 verifier | **当前不能**。`pg_authid` 只有单列 `rolpassword`，且**没有 TOAST 表**【核实 `src/include/catalog/pg_authid.h:31-49`；`src/include/catalog/toasting.h` 与各 catalog 头中的 `DECLARE_TOAST` 列表均无 `pg_authid`】。推荐方案：**给 `pg_authid` 新增一列 `rolpasswordext`** 承载"兼容协议 verifier 列表"，PG 原生路径完全不碰它。 |
| HBA 能否按 protocol_kind 匹配 | **能，改动面小于 100 行**。`check_hba()` 当前对协议**零判定**，任何一行同时管辖三个协议【核实 `src/backend/libpq/hba.c:2530-2631`】。推荐加一个与 `ConnType` **正交**的 `protocol_mask` 字段，用 `protocol=` 选项表达，默认全匹配 → 现有 `pg_hba.conf` 行为零变化。**不要**扩 `ConnType` 枚举。 |
| `ALTER USER ... PASSWORD` 如何分协议设置 | **零语法改动即可**。`password_encryption` 是 `PGC_USERSET`【核实 `src/backend/utils/misc/guc_tables.c:5467`】且枚举里**已经有** `mysql_native_password`【核实 `guc_tables.c:418-422`】。只需把 `user.c` 的写入目标由"永远写 `rolpassword`"改为"按 `target_type` 分流写 `rolpassword` 或 `rolpasswordext`"。 |
| 安全性 | **存在真实的降级风险，但可控**。核心不变量：**角色的有效密码强度 = 其所有 verifier 中最弱的那一个**。必须靠"绝不自动生成弱 verifier" + "verifier 类型与 HBA 方法硬绑定" + "protocol_mask 按协议限制来源"三条来约束。另外发现 **MySQL 侧认证失败时不调用 `ClientAuthentication_hook`**，导致 `auth_delay` 类暴力破解防护在 3306 上完全失效（既有缺陷，见 §8.4）。 |

---

## 1. 现状核实：认证在本内核里是怎么走的

### 1.1 协议分派与认证入口

- 连接方言由 `Port->protocol_kind` 决定，取值 `COMPAT_PROTOCOL_POSTGRES / MYSQL / TDS`【核实 `src/include/libpq/libpq-be.h:84-90, 203-204`】，postmaster 在 accept 时按监听 socket 打标【核实 `src/backend/postmaster/postmaster.c:1966`；`src/backend/libpq/pqcomm.c:193`】。
- 认证的**外层生命周期**（超时、计时、`ClientAuthInProgress`、日志）三协议共用 `PerformAuthentication()`【核实 `src/backend/utils/init/postinit.c:243-244`】。
- 内层分派点是 `ProtocolAuthenticate()`【核实 `postinit.c:200-229`】：
  - 有 `ProtocolRoutine.authenticate` → 走它（MySQL 走这条：`.authenticate = mysql_authenticate`【核实 `contrib/aux_mysql/src/mysql_protocol.c:1645, 668-677`】）；
  - 否则若 `protocol_kind != POSTGRES` → `FATAL`；
  - 否则 → `ClientAuthentication()`。
- **TDS 不走这条路**：它通过自己的 `ProtocolExtensionConfig->fn_authenticate` 分派【核实 `postinit.c:950`；设计说明见 `src/include/postmaster/protocol_routine.h` 中关于 `port->protocol_routine` 恒为 NULL 的注释】。`babelfishpg_tds` 在本仓库中**只有编译产物**（`inst/lib/x86_64-linux-gnu/babelfishpg_tds.so`、`inst/share/extension/babelfishpg_tds*`），**没有源码**【核实：`find` 全仓库仅命中 inst/ 下的二进制与 SQL】。→ TDS 认证改造不在本规格可实施范围（见 §10）。

**关键时序事实**【核实 `postinit.c`】：认证在 `InitPostgres()` 内部第 950 行发生，而 `SetDatabasePath()` 在 1217 行、`RelationCacheInitializePhase3()` 在 1226 行。也就是说**认证时数据库尚未选定**。但 `RelationCacheInitializePhase2()` 已在 868 行执行、`InitCatalogCache()` 在 858 行执行 → **共享目录 + syscache 在认证期可用**（`get_role_password()` 正是这么查 `pg_authid` 的）。这一点决定了 §4 的存储方案可行性边界。

### 1.2 密码存储与校验

- `pg_authid` 结构与社区 18.3 完全一致，只有单列 `rolpassword text`【核实 `src/include/catalog/pg_authid.h:31-49`】。
- **`pg_authid` 没有 TOAST 表**（`DECLARE_TOAST` 列表里无它）。因此 `rolpassword` 无法行外存储，`MAX_ENCRYPTED_PASSWORD_LEN = 512` 的硬上限【核实 `src/include/libpq/crypt.h:26`】既是自我约束也是必要约束。
- `PasswordType` 已有 5 值，含两个 MySQL 值【核实 `crypt.h:56-63`】。
- `get_password_type()` 通过**格式自识别**判定类型：`md5` 前缀 + 长度 + 字符集；SCRAM 用 `parse_scram_secret()`；`mysql_native_password:` 前缀 + 40 位小写 hex；其余一律 `PASSWORD_TYPE_PLAINTEXT`【核实 `crypt.c:96-120`】。
- `get_role_password()` **无任何协议感知**，就是查 `pg_authid` 取 `rolpassword` + 检查 `rolvaliduntil`【核实 `crypt.c:44-91`】。三个调用点：PG 明文口令、PG 挑战口令、MySQL【核实 `auth.c:800, 833`；`mysql_auth.c:783`】。

### 1.3 症状的精确成因（三条硬门，任一都足以造成互斥）

1. **PG 侧拒绝 MySQL 格式**：`plain_crypt_verify()` 对 `PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD` 直接返回 `STATUS_ERROR`【核实 `crypt.c:322-325`】；`CheckPWChallengeAuth()` 只在 `auth_method == uaMD5 && pwtype == PASSWORD_TYPE_MD5` 时走 MD5，否则一律 SCRAM【核实 `auth.c:859-863`】——存了 MySQL 格式就必然掉进 SCRAM 分支并失败。
2. **MySQL 侧拒绝 PG 格式**：`mysql_native_password_verify()` 首行就检查 `get_password_type() != PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD` → `STATUS_ERROR`【核实 `crypt.c:468-473`】。
3. **写入端只有一个槽位**：`CREATE ROLE` / `ALTER ROLE` 都是 `encrypt_password(Password_encryption, ...)` → 写 `Anum_pg_authid_rolpassword`【核实 `src/backend/commands/user.c:445-448, 930-933`】，后写覆盖先写。

补充：`encrypt_password()` 有一条**直通路径**——若传入串已被 `get_password_type()` 识别为非 plaintext，则原样存储、忽略 `password_encryption`【核实 `crypt.c:133-144`】。这意味着 `ALTER USER a PASSWORD 'mysql_native_password:<40hex>'` 会被逐字写入。这既是迁移入口，也是 §8 要评估的攻击面。

### 1.4 HBA 现状

- `ConnType` 6 值：`ctLocal / ctHost / ctHostSSL / ctHostNoSSL / ctHostGSS / ctHostNoGSS`【核实 `src/include/libpq/hba.h:58-66`】，由第一列关键字解析【核实 `hba.c:1366-1421`】——**纯粹是传输层维度**（unix socket / TCP × SSL × GSS 加密态），与"应用层线协议"完全正交。
- `check_hba()` 的匹配维度只有：conntype、SSL 态、GSS 态、IP、database、role【核实 `hba.c:2544-2620`】。**没有任何协议判定** → 一行同时管三个协议。
- `HbaLine` 由 `palloc0()` 分配【核实 `hba.c:1345`】——新增字段若默认语义是"全匹配"，**必须显式初始化**，否则零值 = 全不匹配 = 全实例拒登。
- 尾部选项是 `name=value` 形式，统一由 `parse_hba_auth_opt()` 处理【核实 `hba.c:1856-1887, 2087`】——这是加新维度**成本最低**的挂载点。
- MySQL 连接的 `port->database_name` 被硬设为 GUC `mysql_backend_database`（默认 `"postgres"`）【核实 `mysql_auth.c:637-641`】。→ **一行 `host postgres all 0.0.0.0/0 md5` 同时管辖"PG 连 postgres 库"和"全部 MySQL 连接"**。这是 P3-2 在 HBA 层面的具体碰撞形态。
- MySQL 监听不启 TLS → `port->ssl_in_use == false` → `hostssl` 行永不匹配 MySQL、`hostnossl` 行必然匹配【核实 `hba.c:2556-2567`】。这是**现存的、偶然的**协议区分手段，**不可**当作方案（它把"要求 3306 加密"与"区分协议"绑死了）。
- MySQL 侧只接受 `uaMD5`，`uaReject/uaImplicitReject` 报鉴权失败，其余一律 `FATAL "method not supported"`【核实 `mysql_auth.c:691-710`】。→ **今天 MySQL 对 `trust` 是 fail-closed 的**，这个性质在改造后必须保住。

### 1.5 顺带发现的既有缺陷（不是本方案引入，但必须记录）

| # | 缺陷 | 证据 | 影响 |
|---|---|---|---|
| D1 | `crypt.h:31-34` 注释声称 MySQL 格式"deliberately not exposed through password_encryption"，但 GUC 枚举里**确实暴露了** | 【核实 `crypt.h:31-34` vs `guc_tables.c:418-422`】 | 注释与实现矛盾，误导后续维护者；必须以实现为准并修注释 |
| D2 | `mysql_caching_sha2_password_verify()` 是**死代码**，全仓库无调用者 | 【核实：`grep` 仅命中定义 `crypt.c:517` 与声明 `crypt.h:87`】 | 未接线的认证代码留在树上是审计负担 |
| D3 | `PASSWORD_TYPE_MYSQL_CACHING_SHA2_PASSWORD` 从不被 `get_password_type()` 返回，也不在 `encrypt_password()` / `plain_crypt_verify()` 的 switch 中 | 【核实 `crypt.c:106-119, 147-168, 285-334`】 | 当前**不可触发**（GUC 枚举不含该值）。但若将来加入枚举，`encrypt_password()` 会让 `encrypted_password` 保持 NULL，在 `Assert` 被禁用的 release 构建下直接把 NULL 喂给 `strlen()` → 崩溃【推断，基于 `crypt.c:171` 的 `Assert` 与 `crypt.c:180-181` 的 `strlen`】 |
| D4 | MySQL 认证**失败**路径不调用 `ClientAuthentication_hook` | 【核实 `mysql_auth.c:800-806` 全部是裸 `ereport(FATAL)`；hook 仅在成功路径 `mysql_auth.c:819-820` 调用；对比 PG 侧 `auth.c:663-664` 无论成败都调用】 | **`auth_delay` 等防暴力破解扩展在 3306 上完全失效**。见 §8.4 |
| D5 | MySQL 侧无 mock authentication：用户不存在时立刻 `FATAL`【核实 `mysql_auth.c:784-790`】，而 PG 侧会用 `Password_encryption` 伪装继续走完流程【核实 `auth.c:844-845`】 | 同左 | 3306 存在**用户名枚举**时序侧信道 |
| D6 | `ALTER ROLE ... RENAME` 只在密码是 MD5 时清空【核实 `user.c:1445-1455`】。`mysql_native_password` 的 stage2 是 `SHA1(SHA1(pw))`，**不含用户名 salt**【核实 `crypt.c:415-445`】 | 同左 | 改名后 MySQL 密码继续有效。这与 MySQL 原生语义一致，但与同实例 MD5 语义不一致，需在文档明示 |

---

## 2. 问题一：`get_role_password()` 按协议分派

### 2.1 可行性

**完全可行**。当前实现只做三件事：syscache 查 `pg_authid` → 取 `rolpassword` → 校验 `rolvaliduntil`【核实 `crypt.c:54-90`】。没有任何全局状态耦合，也没有 `Port` 参数。

### 2.2 设计：新增协议感知取值函数

```
/* 新增；不改 get_role_password() 的签名与语义 */
char *get_role_password_ext(const char *role,
                            CompatibilityProtocolKind kind,
                            PasswordType wanted,
                            const char **logdetail);
```

**取值规则（必须严格按此顺序，不得增加任何"再试一个"的回落）**：

| `kind` | 查找顺序 | 找不到时 |
|---|---|---|
| `COMPAT_PROTOCOL_POSTGRES` | **只查** `rolpassword` | 返回 NULL |
| `COMPAT_PROTOCOL_MYSQL` | ① `rolpasswordext` 中类型 == `wanted` 的条目；② 兼容回落：`rolpassword` **且** `get_password_type(rolpassword) == wanted` | 返回 NULL |
| `COMPAT_PROTOCOL_TDS` | 同 MySQL 规则，`wanted` 换成 TDS 的 verifier 类型 | 返回 NULL |

`rolvaliduntil` 检查对所有 `kind` 一律执行（沿用现逻辑，`crypt.c:73-88`）。

**为什么 ② 的"且格式相符"不能省**：这是防降级的硬门。若允许 MySQL 路径拿到 `rolpassword` 里的 SCRAM 串，即使 `mysql_native_password_verify()` 会因格式检查失败（`crypt.c:468-473`），也等于把"格式检查"变成唯一防线。显式的 `wanted` 匹配是第二道门，两道门任一失效都不会造成绕过。

**`get_role_password()` 保留**为 `get_role_password_ext(role, COMPAT_PROTOCOL_POSTGRES, <caller决定>, logdetail)` 的封装，或干脆保持原样只读 `rolpassword`——因为 PG 路径本来就该只看 `rolpassword`。这样 `auth.c:800, 833` 两处调用点**一行都不用改**，PG 原生认证零回归。

**唯一需要改的调用点**：`mysql_auth.c:783`。

---

## 3. 问题二：多 verifier 的存储方案

### 3.1 约束条件（全部已核实）

| 约束 | 来源 |
|---|---|
| C1. 认证发生在数据库选定之前 | `postinit.c:950` vs `1217` |
| C2. 共享目录 + syscache 在认证期可用 | `postinit.c:858, 868`；`get_role_password()` 本身即证明 |
| C3. `pg_authid` **无 TOAST 表** → verifier 不能行外存储 | `DECLARE_TOAST` 列表 |
| C4. 单条 verifier ≤ 512 字节 | `crypt.h:26` |
| C5. `rolpassword` 已有大量既有消费者 | `pg_shadow.passwd`【`system_views.sql:43`】、`pg_dumpall`、`psql \password`、`passwordcheck`、`check_password_hook`【`user.c:398-403, 848-853`】、`RenameRole`【`user.c:1445`】 |
| C6. 新增共享目录若带 syscache，会自动进入共享 relcache init 文件 | `relcache.c:6676-6690`（"any shared relation that's been loaded so far"）、`6831-6847` |

典型长度：md5 = 35 B，SCRAM-SHA-256 ≈ 130 B，mysql_native = 62 B。三份合计 < 250 B，远低于 512。

### 3.2 四个候选方案

#### S1 —— 复用 `rolpassword` 单列，编码成多 verifier 列表

- 做法：把 `rolpassword` 定义成分隔符连接的多条 verifier。
- 优点：零目录改动、零 catversion 变更、`pg_dumpall` 原样往返。
- 缺点：**爆炸半径最大**。必须同时改造 `get_password_type()` 的语义（它现在返回"这一整个串是什么类型"）、`pg_shadow.passwd` 输出、`check_password_hook` 契约、`RenameRole` 的 MD5 清空判定、`encrypt_password()` 的直通路径、`passwordcheck` 扩展、以及所有第三方工具对 `rolpassword` 的解析假设。且违反"三种兼容模式互不干扰"——PG 侧的一个字段被 MySQL 的存在改变了含义。
- **判定：否决。**

#### S2 —— 新增共享目录 `pg_auth_verifier(roleid, verifier_kind, verifier)`

- 做法：新共享 catalog + `(roleid, verifier_kind)` 唯一索引 + syscache。
- 可行性：C6 证明新共享目录带 syscache 会进共享 init 文件，认证期可读【核实 `relcache.c:6676-6690`】。**技术上可行。**
- 优点：最正统、最可扩展（天然支持 per-verifier 的 `valid_until`、锁定计数、创建时间、多 MySQL 插件并存）。
- 缺点：改动面大——新 catalog 头 + `.dat` + OID 分配 + syscache 注册 + `pg_dumpall` 支持 + 权限（必须 `REVOKE ALL FROM PUBLIC`，等同 `pg_authid`）+ `DROP ROLE` 级联清理 + `RenameRole` 联动 + 至少一个新的 `pg_*` 视图。认证路径多一次 syscache 查询。
- **判定：作为"更大改造"的目标形态（§7），本轮不做。**

#### S3 —— `pg_authid` 新增一列 `rolpasswordext text`（**推荐**）

- 做法：在 `rolvaliduntil` 之后追加一个可空 varlena 列，内容为 `\n` 分隔的 verifier 列表。每条 verifier **自带格式前缀**（`md5…` / `SCRAM-SHA-256$…` / `mysql_native_password:…`），因此单条类型可直接用现成的 `get_password_type()` 识别，**无需发明新的编码语法**。
- 优点：
  - **认证路径零额外目录访问**——`get_role_password()` 已经持有 `pg_authid` 的 syscache tuple，多一次 `SysCacheGetAttr()` 即可（对比 S2 的额外 syscache 查询）。
  - **PG 原生路径完全不变**：`rolpassword` 的语义、消费者、视图、dump 格式一个字节都不动 → 直接满足"三种兼容模式互不干扰"。
  - 新列**无历史消费者** → 编码格式可自由定义，不受任何向后兼容约束。
  - `pg_authid.dat` 无需改动：缺失的尾部可空列由 genbki 自动补 `_null_`【推断，依据 `pg_authid.dat` 中 `rolpassword`/`rolvaliduntil` 本身就被省略】。
- 缺点 / 必须处理的点：
  - 需要 catversion bump → **必须 initdb**，不能原地升级。
  - C3（无 TOAST）→ 必须硬限制总长度。建议 **1024 字节上限**并在写入端 `ereport(ERROR)`，远低于"行过大"阈值。
  - `pg_shadow` / `pg_roles` 视图逐列枚举【核实 `system_views.sql:35-48`】→ 新列**不会**被自动暴露。**这是好事**（默认不泄露），但需要单独决定是否新增一个仅超级用户可见的视图。
  - `pg_upgrade` 从社区 PG18 迁入会因 `pg_authid` 列数不同而失败【推断】——但本内核已与社区分叉，此路径本就不成立。
- **判定：推荐，作为本轮最小方案的存储层。**

#### S4 —— 影子角色（`alice` 的 MySQL 口令存在 `alice@mysql` 的 `rolpassword` 里）

- 优点：**零目录改动**、`pg_dumpall` 自动带出、可作为过渡期临时手段。
- 缺点：污染 `pg_authid` 命名空间；影子角色必须 `NOLOGIN` 且需要额外机制保证它不能被当作登录身份或授权主体；`DROP ROLE alice` 不会级联删除影子角色 → 留下孤儿 verifier（**这本身是安全问题**：删了用户，MySQL 侧的 verifier 还在，若日后重建同名角色即复活）。
- **判定：仅作为"不能 initdb 的存量实例"的过渡建议记录，不推荐长期采用。**

### 3.3 推荐存储格式（S3 细则）

```
rolpasswordext (text, 可空, 总长 ≤ 1024)
  = verifier_1 "\n" verifier_2 "\n" ... 
  每条 verifier 原样是当前 encrypt_password() 的输出，自带格式前缀
```

不变式：
- **IV-1**：`rolpasswordext` 中**同一个 `PasswordType` 至多出现一次**。写入时按类型覆盖，不追加。
- **IV-2**：`rolpasswordext` 中**不得出现** `PASSWORD_TYPE_MD5` 或 `PASSWORD_TYPE_SCRAM_SHA_256`。PG 原生格式的唯一归宿是 `rolpassword`。写入端拒绝，读取端忽略（双向校验）。
- **IV-3**：`rolpasswordext` 中**不得出现** `PASSWORD_TYPE_PLAINTEXT`（即无法被 `get_password_type()` 识别的串）。解析到即视为该角色 verifier 集合损坏，**整条拒绝**（fail-closed），并记 `LOG`。
- **IV-4**：`rolvaliduntil` 对所有 verifier 统一生效（不做 per-verifier 过期，那是 S2 的能力）。

---

## 4. 问题三：HBA 的协议维度

### 4.1 不要扩 `ConnType`

`ConnType` 是**传输层**维度（unix/TCP × SSL × GSS），协议方言是**应用层**维度，两者正交。若把它们塞进同一个枚举，取值数会变成 6 × 3 = 18，且第一列关键字要变成 `hostmysqlssl` 之类的组合词。

**判定：新增一个与 `conntype` 并列的独立字段。**

### 4.2 推荐设计

**数据结构**（`src/include/libpq/hba.h`，`HbaLine` 内，`conntype` 附近）：

```
uint32  protocol_mask;   /* bit i = 1 表示匹配 CompatibilityProtocolKind i */
```

**默认值**：`(1u << COMPAT_PROTOCOL_KIND_MAX) - 1`（= 全部协议）。

> ⚠️ **必须在 `parse_hba_line()` 中 `palloc0()` 之后显式赋默认值**【核实 `hba.c:1345`】。零值 = 全不匹配 = 全实例拒登，是一个足以造成生产事故的坑。同时 `check_hba()` 末尾构造的隐式拒绝行也是 `palloc0`【核实 `hba.c:2628`】，那里 mask=0 无所谓（`auth_method` 已是 `uaImplicitReject`）。

**语法**（挂在尾部选项，无需改词法/第一列）：

```
host  all  all  0.0.0.0/0  scram-sha-256  protocol=postgres
host  all  all  10.0.0.0/8 md5            protocol=mysql
host  all  all  0.0.0.0/0  scram-sha-256  protocol=postgres,tds
host  all  all  0.0.0.0/0  reject                                # 无 protocol= → 三协议全管
```

- 解析点：`parse_hba_auth_opt()`【核实 `hba.c:2087`】新增一个 `strcmp(name, "protocol")` 分支，值为逗号分隔的 `postgres|mysql|tds|all`。
- 匹配点：`check_hba()` 循环体最前面（`hba.c:2544` 的 `/* Check connection type */` 之前）插入：
  ```
  if (!(hba->protocol_mask & (1u << port->protocol_kind)))
      continue;
  ```

### 4.3 改动面清单

| 文件 | 改动 | 规模 |
|---|---|---|
| `src/include/libpq/hba.h` | `HbaLine` 加 1 个字段 | 1 行 |
| `src/backend/libpq/hba.c` | `parse_hba_line()` 设默认值 | 1 行 |
| `src/backend/libpq/hba.c` | `parse_hba_auth_opt()` 加解析分支 | ~30 行 |
| `src/backend/libpq/hba.c` | `check_hba()` 加匹配判定 | 3 行 |
| `src/backend/utils/adt/hbafuncs.c` | `fill_hba_line()` 输出新列（`NUM_PG_HBA_FILE_RULES_ATTS` 现为 11）【核实 `hbafuncs.c:184, 202-217`】 | ~10 行 |
| `src/include/catalog/pg_proc.dat` | `pg_hba_file_rules` 返回列 +1 | 1 处 |
| 文档 | `client-auth.sgml` | — |

### 4.4 是否破坏 PG 原生 HBA 语义

**不破坏**。依据：`check_hba()` 目前对协议**零判定**【核实 `hba.c:2530-2631`】，所以"默认 mask = 全匹配"在语义上与现状**逐字节等价**。任何未写 `protocol=` 的现有配置文件行为完全不变。【推断，但基于完整核实的匹配逻辑】

### 4.5 是否需要新的 `UserAuth` 值

**建议需要，但列为第二步（非本轮最小方案）**。

现状的语义错位：MySQL 要求 HBA 方法写 `md5`【核实 `mysql_auth.c:691-694`】，但实际执行的是 mysql_native_password 挑战应答，与 PG 的 MD5 挑战应答毫无关系。管理员看到 `md5` 会误以为是 PG MD5。

目标：新增 `uaMysqlNativePassword`，HBA 名 `mysql_native_password`。连带改动：
- `hba.h` 的 `UserAuth` 枚举 + `USER_AUTH_LAST` 宏【核实 `hba.h:25-44`】
- `hba.c` 的 `UserAuthName[]` 数组，受 `StaticAssertDecl` 保护【核实 `hba.c:102-126`】——数组不同步会**编译期报错**，安全
- `parse_hba_line()` 校验：该方法只允许出现在 `protocol_mask` 仅含 MySQL 的行上
- `auth.c` 的 `ClientAuthentication()` switch 增加显式 `case ... ereport(FATAL)`
- `mysql_auth.c:691-710` 的 switch 从 `uaMD5` 改为 `uaMysqlNativePassword`；过渡期可继续接受 `uaMD5` 但发 `WARNING`

> **安全性利好**：`ClientAuthentication()` 的 `status` 初值是 `STATUS_ERROR`【核实 `auth.c:381`】，且 switch **没有 `default:` 分支**【核实 `auth.c:422-628`】。因此即便忘记加 case，PG 路径遇到新枚举值也会带着 `STATUS_ERROR` 落到 `auth_failed()` —— **天然 fail-closed**。同时 `-Wswitch` 会在编译期告警，不会静默漏掉。

---

## 5. 问题四：`ALTER USER ... PASSWORD` 的分协议设置

### 5.1 推荐：零语法改动，复用 `password_encryption`

依据：`password_encryption` 是 `PGC_USERSET`【核实 `guc_tables.c:5467`】，枚举中**已经包含** `mysql_native_password`【核实 `guc_tables.c:418-422`】。

```sql
-- 设 PG 侧口令（写 rolpassword）
SET password_encryption = 'scram-sha-256';
ALTER USER alice PASSWORD 'S3cret';

-- 设 MySQL 侧口令（写 rolpasswordext），不覆盖上面那条
SET password_encryption = 'mysql_native_password';
ALTER USER alice PASSWORD 'S3cret';
```

**唯一需要的改动**：`user.c` 中的写入分流。

| 位置 | 现状 | 目标 |
|---|---|---|
| `user.c:445-448`（CREATE ROLE） | 永远写 `Anum_pg_authid_rolpassword` | 按 `Password_encryption` 分流 |
| `user.c:930-935`（ALTER ROLE） | 同上 | 同上 |

分流规则：
- `Password_encryption ∈ {MD5, SCRAM_SHA_256}` → 写 `rolpassword`，**`rolpasswordext` 保持不变**
- `Password_encryption ∈ {MYSQL_*}` → 在 `rolpasswordext` 中按类型覆盖对应条目，**`rolpassword` 保持不变**

**关键点**：`encrypt_password()` 的直通路径【核实 `crypt.c:133-144`】意味着当用户直接粘贴一个已编码的 verifier 时，`Password_encryption` 被忽略。因此分流的判据**必须是 `get_password_type(shadow_pass)`（即最终产物的实际类型），而不是 GUC 的值**。否则 `SET password_encryption='scram-sha-256'; ALTER USER a PASSWORD 'mysql_native_password:…'` 会把 MySQL verifier 写进 `rolpassword`，直接破坏 IV-2。

### 5.2 `PASSWORD NULL` 的语义（安全关键）

现状：`user.c:938-943` 只把 `rolpassword` 置 NULL。

**规定：`ALTER USER x PASSWORD NULL` 必须同时清空 `rolpassword` 和 `rolpasswordext`。**

理由：管理员执行"清除密码"时的心智模型是"这个账号不能再用密码登录了"。如果只清了 PG 侧、MySQL 侧 verifier 还在，就是一个静默的认证残留 —— 这类"以为关了其实没关"的缺口是真实事故的主要来源。fail-safe 优先于正交性。

同理，需要一个只清某一侧的显式手段。建议（第二步）：
```sql
SET password_encryption = 'mysql_native_password';
ALTER USER alice PASSWORD NULL;   -- 语义歧义，不推荐用 GUC 表达"清哪个"
```
→ 更好的做法是留给 §7 的 MySQL 方言语法。**本轮：`PASSWORD NULL` 一律全清，不提供部分清除。**

### 5.3 `check_password_hook` 的契约

`check_password_hook` 收到的是**明文密码**与 `get_password_type(password)` 的结果【核实 `user.c:398-403, 848-853`】——注意传的是**入参 password 的类型**，不是最终存储类型。分流改动不影响该 hook 的调用时机与参数，`passwordcheck` 等扩展**行为不变**。但需要在文档中说明：hook 无法区分本次设置的是哪个协议的 verifier。若需要，可在第二步扩展 hook 签名（**属破坏性 ABI 变更**，需谨慎）。

### 5.4 第二步：MySQL 方言语法

```sql
ALTER USER 'alice'@'%' IDENTIFIED WITH mysql_native_password BY 'S3cret';
```
由 MySQL 侧 parser 翻译为内部的 verifier 写入调用。**不在本轮范围**，因为它需要处理 `user@host` 语法与 PG 角色名的映射语义（`'alice'@'%'` 与 `'alice'@'10.%'` 在 PG 里是同一个角色，MySQL 里是两个账号）——这是一个独立的、比认证更大的语义鸿沟。

---

## 6. 本轮可实施的最小方案

**目标验收标准**：同一个角色 `alice`，用同一个明文口令，能同时通过 5432 的 scram-sha-256 和 3306 的 mysql_native_password 登录；且 PG 原生的认证行为、`pg_hba.conf` 语义、`pg_dumpall` 输出**零回归**。

### 6.1 变更清单

| # | 文件 | 变更 | 依赖 |
|---|---|---|---|
| M1 | `src/include/catalog/pg_authid.h` | `rolvaliduntil` 后追加 `text rolpasswordext`（可空，在 `CATALOG_VARLEN` 内） | catversion bump，必须 initdb |
| M2 | `src/include/libpq/crypt.h`<br>`src/backend/libpq/crypt.c` | 新增 `get_role_password_ext()`；新增 verifier 列表的解析/合并/校验辅助函数；实施 IV-1..IV-4 | M1 |
| M3 | `src/backend/commands/user.c` | `445-448` / `930-935` 按**实际 verifier 类型**分流写入；`938-943` 的 `PASSWORD NULL` 改为全清 | M1, M2 |
| M4 | `contrib/aux_mysql/src/mysql_auth.c:783` | 改调 `get_role_password_ext(user, COMPAT_PROTOCOL_MYSQL, PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD, &logdetail)` | M2 |
| M5 | `src/include/libpq/hba.h`<br>`src/backend/libpq/hba.c`<br>`src/backend/utils/adt/hbafuncs.c`<br>`src/include/catalog/pg_proc.dat` | `protocol_mask` 字段 + 默认值 + `protocol=` 选项解析 + `check_hba()` 判定 + `pg_hba_file_rules` 新列 | 独立于 M1–M4，可并行 |
| M6 | `contrib/aux_mysql/src/mysql_auth.c` | 修 D4：失败路径也调用 `ClientAuthentication_hook(port, STATUS_ERROR)` **再** `ereport(FATAL)` | 独立，**建议优先做** |
| M7 | `src/include/libpq/crypt.h:31-34` | 修正 D1 的错误注释 | 独立，纯文档 |

### 6.2 显式不做（留到第二步）

- 不新增 `uaMysqlNativePassword`（§4.5）—— MySQL 侧继续接受 HBA 的 `md5`，语义错位暂留
- 不改 `pg_dumpall`（`rolpasswordext` 暂不参与 dump）→ **必须在发布说明中明确警告**：本轮之后 `pg_dumpall` 不会带出 MySQL 口令，恢复后需重设
- 不做 MySQL mock authentication（D5）
- 不清理 D2/D3 的死代码

### 6.3 实施顺序建议

`M6 → M7 → M5 → M1 → M2 → M3 → M4`

M6 先做的理由：它是纯粹的安全修复，不依赖任何其他改动，且当前 3306 完全没有暴力破解防护。M5 先于 M1 的理由：它不需要 initdb，可以单独验证、单独回滚。

---

## 7. 需要更大改造的部分

| 项 | 内容 | 触发条件 |
|---|---|---|
| G1 | **迁移到 S2（独立共享目录 `pg_auth_verifier`）** | 需要 per-verifier 的 `valid_until` / 失败计数 / 锁定策略 / 密码历史 / 单角色多 MySQL 插件并存时。S3 的单列列表撑不住这些。设计上 S3 → S2 是可平滑迁移的（S3 的列表就是 S2 的行集合） |
| G2 | **`uaMysqlNativePassword` HBA 方法**（§4.5） | 想让 `pg_hba.conf` 自解释、并让 `pg_stat_activity` / `MyClientConnectionInfo.auth_method` 报告真实方法时 |
| G3 | **`caching_sha2_password` 端到端支持** | 需要接 MySQL 8.0+ 默认插件。涉及 D2/D3 的死代码接线、RSA 公钥交换、full-auth 流程、以及 `get_password_type()` 识别 `caching_sha2_password:` 前缀。注意 `mysql_send_greeting()` 已经在广告 `caching_sha2_password`【核实 `mysql_auth.c:175`】却总是 AuthSwitch 到 native【核实 `mysql_auth.c:737`】——先修这个不一致 |
| G4 | **TDS 侧认证** | `babelfishpg_tds` 源码不在本仓库（只有 `.so`）。需要先取得源码，或通过 `ProtocolExtensionConfig->fn_authenticate` 的稳定接口从内核侧提供 verifier 查询 API 供其调用 |
| G5 | **`pg_dumpall` / `pg_upgrade` 支持** | 生产可用性的前置条件。`rolpasswordext` 需要出现在 `ALTER ROLE` 的 dump 语句中，这要求一个新的语法或一个新的设置函数 |
| G6 | **MySQL 方言 `ALTER USER ... IDENTIFIED WITH ... BY`**（§5.4） | 需要先解决 `'user'@'host'` → PG 角色的映射语义 |
| G7 | **MySQL mock authentication**（D5） | 消除用户名枚举侧信道 |
| G8 | **`check_password_hook` 扩展签名** | 若密码策略需要按协议区分强度要求。属破坏性 ABI 变更 |

---

## 8. 安全性分析

### 8.1 威胁模型

- **T1**：可访问 3306 的网络攻击者，尝试绕过 5432 上更强的认证策略
- **T2**：取得 `pg_authid` 内容（备份、`pg_shadow`、只读副本、`pg_dumpall` 输出）的攻击者
- **T3**：拥有 `CREATEROLE` 或可修改自身口令的低权限用户，尝试提权
- **T4**：3306 明文链路上的中间人

### 8.2 核心风险：多 verifier 的强度降级

> **不变量 SEC-0：一个角色的有效密码强度 = 其所有 verifier 中最弱的那一个。**

具体到本方案：

| verifier | 算法 | 加盐 | 迭代 | 离线破解成本 |
|---|---|---|---|---|
| SCRAM-SHA-256 | PBKDF2-HMAC-SHA256 | 是（随机） | 4096 | 高 |
| md5 | `md5(pw \|\| rolname)` | 是（用户名，非随机） | 1 | 低 |
| mysql_native_password | `SHA1(SHA1(pw))`【核实 `crypt.c:415-445`】 | **否** | 1 | **最低，且可彩虹表** |

**风险 R1（T2，离线破解）**：给 `alice` 同时配置 SCRAM 和 mysql_native 之后，攻击者只需破解那个无盐单轮 SHA1 的 stage2，拿到明文后即可登录 5432 的 SCRAM。**SCRAM 的强度被完全旁路。**

> 缓解手段有限，本质上是协议兼容的固有代价。必须做的是：
> - **绝不自动生成**：一次 `ALTER USER ... PASSWORD` **只写一个** verifier。绝不能为了"方便"让一条语句同时写三种格式。这是本方案最重要的单条安全规则。
> - 写入 `PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD` 时 `ereport(WARNING)`，明示"该角色的整体密码强度已降至无盐 SHA1 水平"。
> - 文档明确：不要给超级用户/高权限角色配置 MySQL verifier。

**风险 R2（T1，在线绕过）**：管理员在 `pg_hba.conf` 里对 5432 配了 `scram-sha-256 + hostssl + 内网网段`，但 3306 若未受同等约束，攻击者可直接在 3306 上做在线口令猜测，且用的是更弱的挑战应答。

> **`protocol_mask`（M5）正是为此存在**：它让管理员能对 3306 单独设置来源网段、单独 `reject`。这不是锦上添花，而是本方案的**必要安全组件**——没有它，多 verifier 就是纯粹的风险增加。
>
> 建议在文档中给出推荐配置模板：
> ```
> host  all  all  10.0.0.0/8  md5             protocol=mysql
> host  all  all  0.0.0.0/0   reject          protocol=mysql
> hostssl all all 0.0.0.0/0   scram-sha-256   protocol=postgres
> ```

**风险 R3（verifier 是否密码等价）**：
- `mysql_native_password` 的 stage2 = `SHA1(SHA1(pw))`，而客户端应答需要 `SHA1(pw)`【核实 `crypt.c:454-499`：`stage1 = response XOR SHA1(salt||stage2)`，验证 `SHA1(stage1) == stage2`】。→ **单持有 stage2 无法直接登录**。这比 PG 的 md5（`md5(pw||user)` 就是完整的登录凭据）**更好**。
- 但对离线破解毫无帮助（R1 仍然成立）。

**风险 R4（T4，中间人）**：3306 无 TLS；20 字节 scramble + 20 字节应答足以支撑离线爆破。
> 缓解：文档要求 3306 只监听内网或经由加密隧道；`protocol_mask` 提供了在 HBA 层强制来源限制的手段。

### 8.3 认证绕过的防线（必须逐条在代码中成立）

| ID | 不变量 | 违反后果 | 落点 |
|---|---|---|---|
| **SEC-1** | `protocol_kind == POSTGRES` 的连接**永不读** `rolpasswordext` | MySQL 弱 verifier 被用于 PG 登录 → 完全绕过 SCRAM | `get_role_password_ext()` 的 `kind` 分支（§2.2） |
| **SEC-2** | `protocol_kind != POSTGRES` 的连接**永不**用 `rolpassword` 中的 md5/scram 做认证 | 跨协议凭据混用 | 双门：`wanted` 类型匹配 + 各 `*_verify()` 的格式检查【核实 `crypt.c:230-236, 468-473`】 |
| **SEC-3** | verifier 类型与 HBA 方法**双向硬绑定**。任何"这个 verifier 没有，换一个试试"的回落逻辑都是绕过 | 攻击者可挑最弱的 verifier 打 | `get_role_password_ext()` 只接受精确的 `wanted`，找不到即返回 NULL |
| **SEC-4** | `protocol=` 不匹配时**跳过整行**（`continue`），而非"匹配但换方法" | 可能跌落到后面某条 `trust` 行 | `check_hba()` 的插入点（§4.2） |
| **SEC-5** | `PASSWORD NULL` 清空**全部** verifier | 静默的认证残留 | `user.c:938-943`（§5.2） |
| **SEC-6** | `DROP ROLE` 后不得残留任何 verifier | 同名角色重建即复活旧凭据 | S3 天然满足（同一行 tuple 一起删）；这正是否决 S4 的主因 |
| **SEC-7** | MySQL 侧**保持**对 `trust` / `password` / `peer` / 外部认证的 `FATAL` 白名单拒绝【核实 `mysql_auth.c:691-710`】 | `trust` 行意外管辖 3306 → 无口令登录 | 改造 switch 时只准**增加**明确支持的方法，绝不加 `default: 放行` |
| **SEC-8** | `rolpasswordext` 解析失败（IV-3）时**整条拒绝**，不做部分接受 | 畸形数据被当作某种可通过的凭据 | `get_role_password_ext()` |
| **SEC-9** | `rolpasswordext` **不出现在** `pg_shadow` / `pg_roles` 等 PUBLIC 可见视图中 | 普通用户读到其他角色的 verifier | 视图逐列枚举【核实 `system_views.sql:35-48`】→ 默认已满足，**不要主动加上去** |

**关于"攻击者能否用协议 A 的弱 verifier 绕过协议 B 的强认证要求"的直接回答**：

- **在线绕过：不能**（前提是 SEC-1/SEC-3 成立）。因为 verifier 的选择完全由 `port->protocol_kind` 决定，而 `protocol_kind` 由 postmaster 在 `accept()` 时按**监听 socket** 打标【核实 `postmaster.c:1966`；`pqcomm.c:193`】——**客户端无法自行声明或修改它**。这是本设计最关键的安全支点：协议身份来自服务端的 socket 归属，不来自任何客户端输入。
- **离线绕过：能**（R1）。攻击者破解弱 verifier 得到明文后，可用明文通过任何协议的强认证。这**无法通过代码防御**，只能通过策略（不给高权限角色配弱 verifier）和运维（隔离 3306）缓解。**必须在用户文档中直白写出来，不要淡化。**

### 8.4 已存在的安全缺陷（本方案应顺带修复）

**D4 —— `auth_delay` 在 3306 上完全失效（建议优先修）**

- 证据：MySQL 侧所有失败路径都是裸 `ereport(FATAL)`【核实 `mysql_auth.c:698-701, 705-708, 786-789, 802-805`】，只有成功路径调用 `ClientAuthentication_hook`【核实 `mysql_auth.c:819-820`】。PG 侧则是**无论成败**都调用【核实 `auth.c:663-664`】。
- 后果：`contrib/auth_delay` 通过该 hook 在**失败时**注入延迟【核实 `contrib/auth_delay/auth_delay.c:73-74`】。3306 上这个防护形同虚设 → 该端口可被全速在线爆破，而它恰好用的是最弱的 verifier。**R2 因此被显著放大。**
- 修复：在每个失败 `ereport(FATAL)` 之前插入 `if (ClientAuthentication_hook) (*ClientAuthentication_hook)(port, STATUS_ERROR);`。
- 同样受影响的还有 `sepgsql`【核实 `contrib/sepgsql/label.c:422-423`】及任何依赖该 hook 做审计的扩展。

**D5 —— 3306 用户名枚举**：`get_role_password()` 返回 NULL 即刻 `FATAL`【核实 `mysql_auth.c:784-790`】，与"密码错误"路径的耗时不同（后者多做一次 SHA1 链计算）。PG 侧用 mock 认证规避【核实 `auth.c:844-845`】。属可测量的时序差异。列为 G7。

**D3 —— 潜在的 NULL 解引用**：见 §1.5。当前不可触发，但接 `caching_sha2` 时会踩。修 D3 的正确姿势是给 `encrypt_password()` 的 switch 补全所有枚举值（依靠 `-Wswitch` 而非 `default:`）。

### 8.5 不引入新风险的检查

- **不改** SCRAM / MD5 的算法实现与协议流程 → PG 原生认证强度不变。
- **不改** `rolpassword` 的编码、语义、可见性 → 所有既有工具与视图行为不变。
- `protocol_mask` 默认全匹配 → 现有 `pg_hba.conf` 语义**逐字节等价**（§4.4）。
- 新增 `UserAuth` 值（G2）时，`ClientAuthentication()` 因 `status = STATUS_ERROR` 初值【核实 `auth.c:381`】+ 无 `default:` 分支【核实 `auth.c:422-628`】而**天然 fail-closed**。
- `rolpasswordext` 长度上限 1024 < `pg_authid` 无 TOAST 的行长限制 → 不引入"行过大"的写入失败。

---

## 9. 验收用例

| # | 场景 | 期望 |
|---|---|---|
| A1 | `SET password_encryption='scram-sha-256'; ALTER USER alice PASSWORD 'p';` 然后 `SET password_encryption='mysql_native_password'; ALTER USER alice PASSWORD 'p';` | `psql`（5432, scram）与 `mysql`（3306）**都能登录** |
| A2 | 顺序颠倒执行 A1 | 结果相同 |
| A3 | 只设 scram，尝试 3306 登录 | 拒绝，日志为"无可用 verifier"，**不得**回落到 `rolpassword` |
| A4 | 只设 mysql_native，尝试 5432 登录 | 拒绝，`rolpassword` 为 NULL → 与"用户无密码"行为一致 |
| A5 | `ALTER USER alice PASSWORD NULL` | 5432 与 3306 **都**无法登录 |
| A6 | `DROP ROLE alice; CREATE ROLE alice LOGIN;` | 3306 无法用旧口令登录 |
| A7 | 现有未加 `protocol=` 的 `pg_hba.conf` | 行为与改造前**逐条一致**（回归测试基线） |
| A8 | `host all all 0.0.0.0/0 md5 protocol=mysql` + 5432 连接 | 该行不匹配 → 继续找下一行；找不到则隐式拒绝 |
| A9 | `host all all 0.0.0.0/0 trust protocol=postgres` + 3306 连接 | 该行不匹配 3306；若无其他行则隐式拒绝。**绝不能**让 3306 拿到 `trust` |
| A10 | 3306 上连续输错口令 | `auth_delay` 生效（验证 D4 已修） |
| A11 | 手工把 `rolpasswordext` 改成畸形串 | 两个协议都拒绝登录，日志有明确 `LOG`（SEC-8） |
| A12 | `SET password_encryption='scram-sha-256'; ALTER USER a PASSWORD 'mysql_native_password:<40hex>';` | verifier 依据**实际类型**落入 `rolpasswordext`，`rolpassword` 不受污染（§5.1 关键点） |
| A13 | 普通用户 `SELECT * FROM pg_shadow;` / `pg_roles` | 看不到 `rolpasswordext`（SEC-9） |

---

## 10. 明确不在范围内

以下内容**本规格不涉及**，实施时若触及需另立规格：

1. **TDS / 1433 的认证改造**。`babelfishpg_tds` 在本仓库只有编译产物（`inst/lib/.../babelfishpg_tds.so`、`inst/share/extension/babelfishpg_tds*`），**无源码**。本规格为 TDS 预留了 `protocol_mask` 的位和 `get_role_password_ext()` 的 `kind` 分支，但**不设计、不实现** TDS 侧的任何 verifier 格式或握手流程。
2. **SCRAM / MD5 算法本身的任何修改**，包括迭代次数、盐长度、SASL 流程。
3. **`ConnType` 枚举的扩展**（不新增 `hostmysql` / `hostmysqlssl` 等第一列关键字）。
4. **`rolpassword` 现有编码的任何变更**（S1 已否决）。`pg_shadow.passwd` 的输出格式不变。
5. **`caching_sha2_password` 的端到端支持**，含 RSA 公钥交换与 full-auth 流程（G3）。本规格只记录其死代码现状（D2/D3）。
6. **LDAP / PAM / GSS / RADIUS / cert / OAuth 等外部认证在 MySQL、TDS 协议上的支持**。MySQL 侧保持白名单拒绝（SEC-7）。
7. **`pg_ident.conf` 用户名映射在非 PG 协议上的支持**。
8. **`'user'@'host'` 形式的 MySQL 账号语义**与 PG 角色的映射（G6 的前置问题）。
9. **密码策略**：复杂度校验、历史密码、失败锁定、强制过期。`rolvaliduntil` 沿用现有全局语义（IV-4）。
10. **`pg_upgrade` 从社区 PostgreSQL 18.3 的原地升级**。本内核已与社区分叉，M1 需要 initdb。
11. **`pg_dumpall` 对 `rolpasswordext` 的支持**（G5）。本轮之后 MySQL 口令不参与逻辑备份，**必须在发布说明中警告**。
12. **性能优化**。S3 方案在认证路径不引入额外目录访问，无需专门的性能工作。
13. **任何代码改动**。本文档只产出规格。

---

## 附录 A：核实过的关键代码位置索引

| 主题 | 位置 |
|---|---|
| `PasswordType` 枚举（5 值） | `src/include/libpq/crypt.h:56-63` |
| `MAX_ENCRYPTED_PASSWORD_LEN = 512` | `src/include/libpq/crypt.h:26` |
| 与实现矛盾的注释（D1） | `src/include/libpq/crypt.h:31-34` |
| `get_role_password()` —— 无协议感知 | `src/backend/libpq/crypt.c:44-91` |
| `get_password_type()` —— 格式自识别 | `src/backend/libpq/crypt.c:96-120` |
| `encrypt_password()` 直通路径 | `src/backend/libpq/crypt.c:133-144` |
| `encrypt_password()` switch（缺 CACHING_SHA2） | `src/backend/libpq/crypt.c:147-168` |
| `plain_crypt_verify()` 拒绝 MySQL 格式 | `src/backend/libpq/crypt.c:322-325` |
| `mysql_native_password_encrypt()` —— 无盐双 SHA1 | `src/backend/libpq/crypt.c:415-445` |
| `mysql_native_password_verify()` —— 格式硬门 | `src/backend/libpq/crypt.c:468-473` |
| `mysql_caching_sha2_password_verify()` —— 死代码 | `src/backend/libpq/crypt.c:517-569` |
| `ClientAuthentication()` `status` 初值 / 无 `default:` | `src/backend/libpq/auth.c:381 / 422-628` |
| `CheckPWChallengeAuth()` 的类型分派 | `src/backend/libpq/auth.c:833-863` |
| `ClientAuthentication_hook` 无条件调用（对比 D4） | `src/backend/libpq/auth.c:663-664` |
| `UserAuth` 枚举 / `USER_AUTH_LAST` | `src/include/libpq/hba.h:25-44` |
| `ConnType` 枚举（6 值） | `src/include/libpq/hba.h:58-66` |
| `HbaLine` 结构 | `src/include/libpq/hba.h:95-143` |
| `UserAuthName[]` + `StaticAssertDecl` | `src/backend/libpq/hba.c:102-126` |
| `parsedline = palloc0()` | `src/backend/libpq/hba.c:1345` |
| 第一列 ConnType 解析 | `src/backend/libpq/hba.c:1366-1421` |
| 尾部 `name=value` 选项循环 | `src/backend/libpq/hba.c:1856-1887` |
| `parse_hba_auth_opt()` | `src/backend/libpq/hba.c:2087` |
| `check_hba()` —— 无协议判定 | `src/backend/libpq/hba.c:2530-2631` |
| `fill_hba_line()` / `NUM_PG_HBA_FILE_RULES_ATTS=11` | `src/backend/utils/adt/hbafuncs.c:184, 202-217` |
| `pg_authid` 单列 `rolpassword` | `src/include/catalog/pg_authid.h:31-49` |
| `pg_shadow` 视图逐列枚举 | `src/backend/catalog/system_views.sql:35-48` |
| 共享 relcache init 文件规则 | `src/backend/utils/cache/relcache.c:6676-6690, 6831-6847` |
| `password_encryption` 枚举含 mysql | `src/backend/utils/misc/guc_tables.c:418-422` |
| `password_encryption` 为 `PGC_USERSET` | `src/backend/utils/misc/guc_tables.c:5467` |
| CREATE ROLE 写密码 | `src/backend/commands/user.c:445-448` |
| ALTER ROLE 写密码 | `src/backend/commands/user.c:930-935` |
| `PASSWORD NULL` 清空 | `src/backend/commands/user.c:938-943` |
| `check_password_hook` 调用点 | `src/backend/commands/user.c:398-403, 848-853` |
| `RenameRole` 只清 MD5 | `src/backend/commands/user.c:1445-1455` |
| `CompatibilityProtocolKind` 枚举 | `src/include/libpq/libpq-be.h:84-90` |
| `Port->protocol_kind` / `protocol_routine` | `src/include/libpq/libpq-be.h:203-204` |
| postmaster 按监听 socket 打标 | `src/backend/postmaster/postmaster.c:1966`；`src/backend/libpq/pqcomm.c:193` |
| `ProtocolAuthenticate()` 分派 | `src/backend/utils/init/postinit.c:200-229` |
| 认证 vs 数据库选定时序 | `src/backend/utils/init/postinit.c:950` vs `1217, 1226`；`858, 868` |
| MySQL `.authenticate` 注册 | `contrib/aux_mysql/src/mysql_protocol.c:1645, 668-677` |
| MySQL `port->database_name` 硬设 | `contrib/aux_mysql/src/mysql_auth.c:637-641` |
| MySQL HBA 方法白名单（仅 uaMD5） | `contrib/aux_mysql/src/mysql_auth.c:691-710` |
| MySQL 取密码 + 校验 | `contrib/aux_mysql/src/mysql_auth.c:783-806` |
| MySQL hook 仅成功时调用（D4） | `contrib/aux_mysql/src/mysql_auth.c:819-820` |
| MySQL 广告 caching_sha2 但总切 native | `contrib/aux_mysql/src/mysql_auth.c:175, 737` |
| `auth_delay` 依赖的 hook | `contrib/auth_delay/auth_delay.c:73-74` |
