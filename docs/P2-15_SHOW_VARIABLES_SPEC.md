# P2-15 实施规格：MySQL `SHOW VARIABLES` 系统性返回空值

## ⛔ 结论已作废：本项不做（2026-08-14 上游比对核实）

**用户原则**：融合阶段若某兼容特性 **openHalo 和 Babelfish 上游本身也没实现**，则不做修复和实现，保持与上游兼容特性一致即可。

**核对结果（代码核实，逐项字符级比对 openHalo 源仓库 `/home/hlv/openhalo-update/postgres`，分支 `openhalo-update`）**：

1. `contrib/aux_mysql/aux_mysql--1.3--1.4.sql` 里 `empty_session_variables`/`empty_global_variables` 两个视图的定义，与本融合树**字符级完全一致**（均为 `value` 列硬编码为 `''::varchar(1024)`）。该文件是提交 `bcee6d44b5`（`fusion: import openHalo's self-contained MySQL compatibility files`）**原样导入**的 158 个 openHalo 自包含文件之一，未经任何修改——`git log -S "empty_session_variables"` 只有这一个提交也印证了这点。
2. `src/backend/parser/mysql/mys_gram.y` 里全部 8 条 `SHOW VARIABLES` 系列语法规则，硬编码指向这两个空视图的写法与本融合树完全一致（仅因文件头版权声明差异导致行号偏移 29 行，逻辑和数量完全对应）。
3. `src/backend/adapter/mysql/systemVar.c` 的 `getSystemVariableValueForShow()` 在 openHalo 源里同样**只有定义和头文件声明、零调用点**，是死代码——与本融合树状态一致。

**即 openHalo 上游本身就是"用空 value 占位视图应付 `SHOW VARIABLES`"的设计，这不是融合过程引入的回归。**

**因此下文 §5 的推荐方案（内核加枚举函数 + mysm 加 SRF + 视图重定义 + 语法层收敛）全部作废** —— 那属于超越上游去实现 openHalo 自己都没做的特性，不符合本项目"三协议融合、保持上游兼容特性不变"的目标。

子代理产出本文档时，曾出现过一处表述上的自相矛盾：一方面用 `git log -S` 证据指出该视图是"上游原样导入"，另一方面又给出了完整的 21.5 小时实施方案。现已确认前者（原样导入、上游本就如此）是正确的判断，后者应当废弃。

**本文档以下内容仅作为技术档案保留**（记录了当前实现范围、根因链路、以及若将来上游修复或用户改变范围决策时的实施方案参照），**不构成行动建议**。

---

- 仓库：`/home/hlv/openhalo-update/postgresql_modified_for_babelfish`
- 日期：2026-08-13
- 状态：分析完成，待实施（本文档不含任何代码改动）
- 前置：P2-10（事务隔离级别读路径接真实 GUC）、P2-16（MySQL 会话默认隔离级别）已完成，其教训是本方案的设计基础

每条结论标注 **［代码核实］**（附 `文件:行号`）或 **［推断］**。

---

## 1. 结论摘要

| 项 | 结论 |
|---|---|
| 根因 | `SHOW VARIABLES` 在语法层被改写成查两个**硬编码空串**的视图；真正的读取入口 `getSystemVariableValueForShow()` 从未接线，是死代码 |
| 影响面 | 全部 199 个变量，4 种作用域写法 × 2 种过滤写法 = 8 条语法规则，全部受影响 |
| 次生缺陷 | 至少 6 个（见 §4）：`version` 根本不在目录表里、列名大小写不符 MySQL、无排序、`max_allowed_packet` 三处取值互不一致、`getConfValue()` 有返回 NULL 的漏洞、全局哨兵行会被枚举出来 |
| 推荐方案 | 内核加枚举函数 → `mysm` 加 SRF → 扩展脚本**原地重定义**两个视图 → 语法层 8 条规则收敛到一个 helper（**不新增非终结符**，零 LALR 冲突风险） |
| 关键约束 | 读值来源必须遵守「读写对称原则」（§6）：只有写路径真正作用于后端的变量才允许读真实后端状态，否则继续读 MySQL 自有存储 |
| 工作量 | 约 1.5～2.5 人日（含测试与三方言回归） |
| 风险 | 中低。最大风险是「值变了」本身——从空串变成真值可能触发客户端此前未走到的分支 |

---

## 2. 现状：`SHOW VARIABLES` 的完整路径

### 2.1 语法层（8 条规则，全部指向空视图）

**［代码核实］** `src/backend/parser/mysql/mys_gram.y`，8 条规则各自手写了几乎相同的 20 行 `SelectStmt` 构造代码：

| 行号 | 语法 | 改写目标视图 |
|---|---|---|
| 2555 | `SHOW VARIABLES LIKE 'pat'` | `mys_informa_schema.empty_session_variables` |
| 2583 | `SHOW LOCAL VARIABLES LIKE 'pat'` | `empty_session_variables` |
| 2611 | `SHOW SESSION VARIABLES LIKE 'pat'` | `empty_session_variables` |
| 2639 | `SHOW GLOBAL VARIABLES LIKE 'pat'` | `empty_global_variables` |
| 2667 | `SHOW VARIABLES [WHERE …]` | `empty_session_variables` |
| 2691 | `SHOW LOCAL VARIABLES [WHERE …]` | `empty_session_variables` |
| 2715 | `SHOW SESSION VARIABLES [WHERE …]` | `empty_session_variables` |
| 2739 | `SHOW GLOBAL VARIABLES [WHERE …]` | `empty_global_variables` |

每条规则构造的都是 `SELECT * FROM <view> [WHERE variable_name ~~* 'pat' | <用户 WHERE>]`。

**［代码核实］** `where_clause` 在 PG 语法中可为空（`mys_gram.y` 复用标准 `where_clause` 规则），因此**裸 `SHOW VARIABLES`（无 LIKE 无 WHERE）走的是 2667 行这条规则**，返回全部 199 行。

**［代码核实］** `LOCAL` 与 `SESSION` 都映射到 session 视图（2583/2611、2691/2715），这与 MySQL 语义一致（`LOCAL` 是 `SESSION` 的同义词），**这一点当前是正确的，不需要改**。

### 2.2 视图层（硬编码空串）

**［代码核实］** `contrib/aux_mysql/aux_mysql--1.3--1.4.sql:3531-3543`：

```sql
create view mys_informa_schema.empty_session_variables as
select
    variable_name::varchar(128) as variable_name,
    ''::varchar(1024) as value          -- ← 对所有变量返回空串
from mys_informa_schema.base_variables;
```

`empty_global_variables`（3538-3543 行）定义**逐字相同**——连 `session`/`global` 的区分都没有，两个视图完全等价。

**［代码核实］** 数据源 `mys_informa_schema.base_variables`（表定义 `aux_mysql--1.3--1.4.sql:3319-3332`，199 条 `INSERT`）字段为：
`variable_name, def_value, conf_value, sess_global_type, sess_def_val_from_global, is_read_write, valid_result, select_result_rule, show_result_rule`。

**注意：`conf_value` 列里其实是有值的**——视图只是没有 `select` 它。即便只把 `''::varchar(1024)` 改成 `conf_value`，得到的也是**启动时的种子值**而非会话当前值，仍然是错的（`SET` 后不会变）。这一点决定了本问题不能靠改一行 SQL 解决。**［推断］**

### 2.3 结果

**［代码核实 + 推断］** `SHOW VARIABLES LIKE 'version%'` 的实际返回：由于 `base_variables` 里根本**没有 `version` 这一行**（只有 `version_comment`，3425 行），该语句返回**恰好 1 行**：`version_comment` / `''`。用户观察到的「值为空」只是症状之一，行集本身也是错的。

### 2.4 对照：`SELECT @@varname` 为什么能工作

**［代码核实］** 走的是完全不同的一条路径：

```
@@name  →（mys_gram.y 降级）→ pg_catalog.mys_get_system_variable(name, bool)
        → src/backend/commands/mysql/mys_uservar.c:128
        →（先查一批 override 快速返回）
        → getSystemVariableValueForSelect()  [systemVar.c:405]
        → getSystemVariableValueImpl(..., selectShowType=1, ...)  [systemVar.c:1710]
        → sessionSystemVars / globalSystemVars 哈希表
```

**［代码核实］** `pg_catalog.mys_get_system_variable` 在 `src/include/catalog/pg_proc.dat:1641` 声明为 `provolatile => 'v'`（VOLATILE），因此不会被计划期常量折叠——这是 `@@` 路径能反映会话当前状态的前提。

**［代码核实］** `mys_uservar.c:137-207` 的 override 链目前覆盖：`autocommit`（读真实会话状态 `MysAutocommitEnabled()`）、`character_set_*` / `collation_*`（静态字面量）、`max_allowed_packet`（静态 `"16777216"`）、`version` / `version_comment`（GUC `mysql_server_version`）、`time_zone`（`MysGetSessionTimeZone()` / `MysGetGlobalTimeZone()`）、`transaction_isolation`（GUC `default_transaction_isolation` + `MysIsoLevelToMysql()`，P2-10 引入，注释在 `mys_uservar.c:186-197`）。

**这条 override 链是 `@@` 路径独占的。`SHOW` 路径即使接通哈希表，也拿不到这些 override 值** ——这是本次修复必须解决的核心一致性问题（见 §6）。**［推断，依据是 override 链写死在 `mys_get_system_variable()` 函数体内，无任何复用出口］**

### 2.5 为什么当初设计成返回空串

**［代码核实］** `git log -S "empty_session_variables"` 只有一个提交：`bcee6d44b5 fusion: import openHalo's self-contained MySQL compatibility files`。也就是说**这两个视图是从上游 openHalo 原样导入的，本融合项目从未改过**，代码中也没有任何解释性注释。

**［推断］** 从命名（`empty_` 前缀是刻意的，不是笔误）和 `getSystemVariableValueForShow()` 已经写好但没接线这两点看，上游的意图应该是：先用一个「行集正确、值为空」的占位视图让 `SHOW VARIABLES` 不报语法错（很多 MySQL 客户端/驱动在连接握手阶段会发 `SHOW VARIABLES`，报错会直接连不上），把取值留到后面做，结果没做完。**没有找到任何证据表明「返回空串」是有意的兼容性决策**——恰恰相反，`getSystemVariableValueForShow()` 的存在证明原计划就是要返回真值。

---

## 3. `ForShow` 与 `ForSelect` 的差异

**［代码核实］** 两个函数都只是 `getSystemVariableValueImpl()` 的薄封装，唯一区别是第三个参数 `selectShowType`：

| 函数 | 位置 | `selectShowType` | 返回值 |
|---|---|---|---|
| `getSystemVariableValueForSelect` | `systemVar.c:405-428` | `1` | `void` |
| `getSystemVariableValueForShow` | `systemVar.c:431-455` | `2` | `bool`（**永远只可能返回 `true`**） |

**［代码核实］** `getConfValue()`（`systemVar.c:1786-1832`）对该参数的使用：

```c
if (selectShowType == 1)          /* SELECT 路径 */
{
    if (systemVar->selectResultNum == 0)
        ret = pstrdup(systemVar->varConfValue);      /* 无映射 → 原样 */
    else
        /* 用 varConfValue 在 result[] 里找下标 i，返回 selectResult[i] */
}
else                              /* SHOW 路径（2） */
{
    if (systemVar->showResultNum == 0)
        ret = pstrdup(systemVar->varConfValue);
    else
        /* 用 varConfValue 在 result[] 里找下标 i，返回 showResult[i] */
}
```

**语义**：`base_variables` 的三列 `valid_result` / `select_result_rule` / `show_result_rule` 构成一张展示映射表，装进 `SystemVar` 结构（`systemVar.c:301-315`）的 `result[]` / `selectResult[]` / `showResult[]` 三个并行数组。存储值 `varConfValue` 永远是 `result[]` 里的某一项，`SELECT @@x` 和 `SHOW VARIABLES` 各自映射到自己那一列。

典型例子 **［代码核实］** `aux_mysql--1.3--1.4.sql:3333`：
```sql
insert into base_variables values('autocommit', '1','1', 0, true,true, '0|1', null, 'OFF|ON');
--                                                                     result  select  show
```
即 `autocommit` 存储 `'1'`，`SELECT @@autocommit` 应返回 `1`（无 select 映射 → 原样），`SHOW VARIABLES LIKE 'autocommit'` 应返回 `ON`。**这正是 MySQL 的真实行为**——所以 `showResult` 这套机制是必要的，不能用 `ForSelect` 顶替 `ForShow`。

**［代码核实］缺陷**：`getConfValue()` 在「有映射但 `varConfValue` 不在 `result[]` 里」时，`ret` 保持 `NULL` 返回。调用方 `getSystemVariableValueImpl` 不检查，一路传出去。`@@` 路径下表现为返回 SQL NULL；SHOW 路径下会变成 NULL 行。**修复时必须顺手补一个回退到 `varConfValue` 的分支。**

**［代码核实］另一处**：`getSystemVariableValueForShow()` 返回类型是 `bool`，但函数体里只有 `return true` 一条返回路径，其余两条都是 `elog(ERROR)`。原设计显然想让 SHOW 路径「读不到就跳过这一行」（返回 `false`），但**这个跳过语义从未实现**。这直接说明：即使把这个函数接线到 SHOW 路径，逐行调用它仍会在遇到作用域不匹配的变量时**整条语句报错**（见 §7）。

---

## 4. 同时暴露的次生缺陷清单

修复时必须一并处理，否则「修好了但还是不对」：

| # | 缺陷 | 证据 | 处理 |
|---|---|---|---|
| N1 | `version` 根本不在 `base_variables` 里 | `grep "base_variables values('version'"` 无结果；只有 `version_comment`（3425 行） | 必须补行（`port`/`hostname`/`datadir`/`basedir` 同样缺失，本次只补 `version`，其余见 §9） |
| N2 | 列名是 `variable_name`/`value`，MySQL 是 `Variable_name`/`Value` | 8 条规则都用 `SELECT *`（如 `mys_gram.y:2560-2571`）；对照 `SHOW STATUS` 用了 `createResTargetWithColumn("Variable_name", "variable_name", …)`（`mys_gram.y:3331-3332`） | 语法层加别名，照抄 SHOW STATUS 写法 |
| N3 | 无排序，行序不确定 | 8 条规则都没有 `sortClause`；`SHOW STATUS` 有（`mys_gram.y:3346-3350`） | 加 `ORDER BY 1`，兼顾 MySQL 语义与测试输出稳定 |
| N4 | `max_allowed_packet` 三处取值互不一致 | GUC `mysql_max_allowed_packet = 64*1024*1024`（`guc_tables.c:585`）／目录 `conf_value='536870912'`（`aux_mysql--1.3--1.4.sql:3377`）／`@@` override 静态 `"16777216"`（`mys_uservar.c:155`） | 统一到 GUC，三处收敛为一处 |
| N5 | `getConfValue()` 可返回 NULL | `systemVar.c:1786-1832`，见 §3 | 加 `varConfValue` 回退 |
| N6 | 全局哈希表里有个哨兵条目会被枚举出来 | `initGlobalSystemVars()` 往 `globalSystemVars` 里 `HASH_ENTER` 了一个名为 `"global_system_vars_init_flag"` 的假变量（`systemVar.c:934-944`） | 枚举时按名字跳过 |
| N7 | `mysql.get_system_variable()` 被声明为 `IMMUTABLE` | `aux_mysql--1.3--1.4.sql:3641-3644`，而内核同名函数是 `provolatile='v'`（`pg_proc.dat:1641`） | 顺手改成 `VOLATILE`（低风险，防止将来被折叠） |

---

## 5. 修复方案对比与推荐

### 方案 A：视图改成读 `conf_value` 列
一行 SQL 改动。**否决**——`conf_value` 是启动种子值，`SET` 之后不变，等于把「全空」换成「全部撒谎」。按 P2-10 的教训，**主动误导比返回空更糟**。

### 方案 B：视图里逐行调用已有标量函数
`select variable_name, mysql.get_system_variable(variable_name, false) from base_variables`。**否决**，三个硬伤：
1. **［代码核实］** `getSystemVariableValueForSelect()` 在变量作用域不匹配时 `elog(ERROR)`（`systemVar.c:420-426`），`SHOW GLOBAL VARIABLES` 遇到第一个 session-only 变量（`sess_global_type=2`）就会**整条语句报错**；
2. 走的是 SELECT 映射（`selectResult`）而非 SHOW 映射（`showResult`），`autocommit` 会显示 `1` 而不是 MySQL 的 `ON`；
3. 199 次函数调用，每次 GLOBAL 查询都要抢一次自旋锁。

### 方案 C：语法层直接改走 `VariableShowStmt` 之类的内核 SHOW 路径
**否决**——`VariableShowStmt` 是 PG 的 GUC 展示节点，语义是「一个变量一行值」，且它读的是 PG GUC 而非 MySQL 变量存储，方向完全不对。

### 方案 D（推荐）：内核枚举函数 + `mysm` SRF + 视图原地重定义 + 语法层收敛

```
SHOW [GLOBAL|SESSION|LOCAL] VARIABLES [LIKE 'p' | WHERE …]
  │
  ├─ mys_gram.y: mysqlMakeShowVariables(isGlobal, likePattern, whereClause)
  │     构造 SELECT variable_name AS "Variable_name", value AS "Value"
  │            FROM mys_informa_schema.empty_{session,global}_variables
  │            [WHERE …] ORDER BY 1
  │
  ├─ 视图（aux_mysql 1.6→1.7 原地 CREATE OR REPLACE）
  │     → SELECT * FROM mysql.show_variables(<bool>)
  │
  ├─ mysm SRF: showSystemVariables()   (materialize 模式)
  │
  └─ 内核: MysEnumerateSystemVariables(bool isGlobal)
        ├─ 名单来自 baseSystemVars[]（后端本地数组）
        ├─ 作用域过滤（§7）
        ├─ 值 = MysResolveSystemVarOverride() ?? getConfValue(sv, /*show*/2)
        └─ override 与 @@ 路径共用同一个函数（§6，关键）
```

**推荐理由：**

1. **有直接先例。** **［代码核实］** `SHOW WARNINGS` 走的就是「语法层 → 扩展里的 SRF」这条路：`mys_gram.y:2864-2880` → `mysql.show_warnings()`（`aux_mysql--1.5--1.6.sql:160-163`）→ `mysql_show_warnings()`（`contrib/aux_mysql/src/mysql_protocol.c:1725-1783`）。SRF 的 materialize 写法可以直接抄。
2. **保留视图这一层是刻意的，不是多余的间接。** 语法层（内核二进制）与扩展 SQL 是两个独立升级的部件。如果语法层直接引用 `mysql.show_variables()`，那么「新内核 + 旧扩展」的组合会让 `SHOW VARIABLES` 直接报 `function does not exist`；保留视图名不变，则旧扩展下退化为今天的行为（返回空值），**只错值不断功能**。
3. **视图名不改、列名列类型不改** → 用 `CREATE OR REPLACE VIEW` 即可原地替换，无需 `DROP`，不影响任何依赖对象。**［代码核实］** 现视图列为 `variable_name varchar(128)` / `value varchar(1024)`，新定义保持一致。
4. **语法层不新增非终结符。** 8 条规则的规则头一字不动，只把各自的规则体换成一行 helper 调用。**这是刻意的**：`SHOW STATUS` 用的 `show_session_global_opt`（`mys_gram.y:4051-4055`）是个无值规则，要给 VARIABLES 复用就得新造一个带返回值的作用域规则，而 `SHOW GLOBAL VARIABLES` 与 `SHOW GLOBAL STATUS` 需要两个 token 的前瞻才能决定归约哪个空规则——**LALR(1) 下极可能产生冲突**。不动规则结构就完全规避了这个风险。**［推断，基于 LALR(1) 归约时机分析；实施时若仍想合并，必须先跑 `bison -Wcounterexamples` 验证］**
5. **枚举名单取自 `baseSystemVars[]` 而非遍历共享内存哈希表**，规避了一个真实的实现陷阱：**［代码核实］** `lockGlobalSystemVars()` 用的是 `SpinLockAcquire`（`systemVar.c:1059-1076`），而 PG 明令禁止在自旋锁下做 `palloc` / `ereport` / 长循环。对 199 个条目做 `hash_seq_search` + `pstrdup` 会直接违反这条约束。改成「本地数组出名单，逐个变量走已有的短临界区查询」既安全又不用写新的加锁代码。

---

## 6. 关键：值的来源分层，以及如何不再制造「读到的值与真实后端脱节」

这是本次修复最容易出错、也是 P2-10 唯一真正的教训所在。

### 6.1 强制规则：读写对称原则

> **一个变量的 SHOW 值允许来自后端状态 X，当且仅当对该变量执行 `SET` 时写入的也是 X（或该变量只读、根本无写路径）。**
>
> 违反这条规则的两种方向都是有害的：
> - 只写不读 → 「设了不生效的假象」（P2-10 修复前的 `transaction_isolation`）
> - 只读不写 → 「读得到但设不动」（P2-10 规格 §5 项 B 明确拒绝的做法）

### 6.2 写路径实际作用于后端的变量（可以且必须读真实状态）

**［代码核实］** `applySystemVarValue()`（`systemVar.c:1558-1697`）**只对 4 个变量有真实副作用**，其余全部落到 `else { /* do nothing; */ }`：

| 变量 | 写路径做了什么 | 读路径应取自 | 核实位置 |
|---|---|---|---|
| `autocommit` | 改会话状态 `autoCommit` / `needCommitTrx` / `needStartNewTrx` | `MysAutocommitEnabled()` | `systemVar.c:1560-1600`；读侧已有 `mys_uservar.c:143-146` |
| `time_zone` | 构造 `VariableSetStmt` 调 `ExecSetVariableStmt()` 真改 PG `timezone` GUC | `MysGetSessionTimeZone()` / `MysGetGlobalTimeZone()` | `systemVar.c:1600-1626`；读侧 `mys_uservar.c:167-183` |
| `sql_mode` | 解析进全局 `mys_sqlMode` 位掩码 | `varConfValue`（见下方警告） | `systemVar.c:1626-1673` |
| `default_week_format` | 写全局 `default_week_format` | `varConfValue`（写入前已校验，两者一致） | `systemVar.c:1673-1697` |

外加 P2-10 特例化的 `transaction_isolation`：写路径在 `contrib/aux_mysql/src/mys_utility.c` 处理，读路径取 GUC `default_transaction_isolation` 并经 `MysIsoLevelToMysql()` 转格式 **［代码核实］** `mys_uservar.c:186-207`。

⚠ **`sql_mode` 的已知残留偏差**：**［代码核实］** `systemVar.c:1643-1647` 把 `STRICT_TRANS_TABLES` 的解析结果**硬写成 `false`**（`//isStrictTransTablesOn = true;` 被注释掉了）。所以显示 `varConfValue` 会声称严格模式开着，实际没开。这是真实的「读写脱节」，但**改显示值会影响客户端行为判断，风险高于本次修复范围** → 列入 §9 不在范围内，但必须在 FUSION_PLAN 里单独记一条。

### 6.3 只读且有真实 GUC 对应的变量（可以读真实值，无对称性风险）

只读变量不存在「设不动」的问题，接真实 GUC 是纯收益：

| 变量 | 真实来源 | 核实位置 | 当前状况 |
|---|---|---|---|
| `version` | GUC `mysql_server_version`（`"8.4.10-openhalo-1.0"`） | `guc_tables.c:581`；`@@` 侧已用 `mys_uservar.c:159-161` | 目录表里**根本没有这一行**（N1） |
| `version_comment` | 目录值 `'MySQL Server (GPL)'` 即可 | `aux_mysql--1.3--1.4.sql:3425` | 但 `@@` 侧 override 错误地也返回了 `mysql_server_version`（`mys_uservar.c:159-160` 把两个名字并到了一个分支），**顺手修正** |
| `max_allowed_packet` | GUC `mysql_max_allowed_packet` | `guc_tables.c:585`、`guc.h:300` | 三处不一致（N4） |

### 6.4 值只存在于 MySQL 自有存储的变量（绝大多数，保持现状）

**［推断］** 199 个变量里的绝大部分（`sql_log_bin`、`auto_increment_increment`、`query_cache_size`、`innodb_*`、`slow_query_log*` 等）在 PG 里没有任何对应概念。它们的 `SET` 只改 `varConfValue`，`SHOW` 也只应读 `varConfValue`（经 `showResult` 映射）。**这是自洽的**：写什么读什么，没有第三方状态可以与之矛盾。

### 6.5 ⛔ 明确禁止顺手接线的变量

这些变量在 PG 里**看似**有对应物，但写路径完全没实现。接了读路径就会立刻制造出「读得到但设不动」——正是 P2-10 规格 §5 项 B 拒绝过的反模式：

| 变量 | 看似对应的 PG 状态 | 为什么不接 |
|---|---|---|
| `lower_case_table_names` | 标识符折叠策略 | **［代码核实］** 全仓库 `grep` 无任何 C 代码读它，写了也没人用 |
| `read_only` / `super_read_only` | `default_transaction_read_only` | `applySystemVarValue()` 无分支，`SET` 不生效 |
| `character_set_server` / `collation_server` | `server_encoding` / `lc_collate` | 真实库编码未必是 utf8mb4；接了会让客户端拿到与 `@@` 路径不同的答案，且 `SET` 依然无效 |
| `port` / `datadir` / `basedir` / `hostname` | `mysql_port` GUC 等 | 目录表里根本没有这些行；补行 + 接 GUC 是独立的「补齐只读环境变量」任务，与本 bug 无关 |

### 6.6 落地办法：override 链必须提取成共享函数

**这是防止再次脱节的机制性保障，不是可选项。**

**［代码核实］** 当前 override 链写死在 `mys_get_system_variable()` 的函数体里（`mys_uservar.c:137-207`），无任何复用出口。如果 SHOW 路径另写一份 override，两条路径**必然随时间发散**——今天 `max_allowed_packet` 已经有三个不同的值就是活证据（N4）。

**要求**：把 override 链提取到 `src/backend/adapter/mysql/systemVar.c`，形如

```c
/* 返回 NULL 表示「无 override，请走常规存储」 */
const char *MysResolveSystemVarOverride(const char *name, bool isSession, bool forShow);
```

然后 `mys_get_system_variable()`（`@@` 路径）和 `MysEnumerateSystemVariables()`（SHOW 路径）**都只调用它**。`forShow` 参数用于区分需要 SHOW 映射的少数项（如 `autocommit` 在 `@@` 下是 `1`、在 SHOW 下是 `ON`）。

**［代码核实］顺带修正的名字规范化顺序问题**：现在 `mys_get_system_variable()` 是「先查 override（override 分支里手工枚举了 `session.autocommit` / `local.autocommit` 等带前缀写法，但漏了 `global.autocommit`），再剥前缀」（override 链在 `mys_uservar.c:137-207`，剥前缀在 `mys_uservar.c:210-226`）。提取时改成**先规范化名字与作用域、再查 override**，一次性消除这类漏写。

---

## 7. `SHOW GLOBAL VARIABLES` vs `SHOW SESSION VARIABLES`

### 7.1 底层已有的作用域机制

**［代码核实］** `sess_global_type` 取值语义（`aux_mysql--1.3--1.4.sql:3325-3326` 的注释）：

| 值 | 含义 |
|---|---|
| 0 | global & session |
| 1 | global only |
| 2 | session only |
| 3 | session only but can select global |

**［代码核实］** `getSystemVariableValueImpl()`（`systemVar.c:1710-1782`）按 `isSessionSystemVar` 分流到两个哈希表：
- session 分支：直接查 `sessionSystemVars`，**不做类型检查**
- global 分支：查 `globalSystemVars`，**只接受 type ∈ {0,1,3}，type==2 返回 `ret=2`** → 上层 `elog(ERROR, "Variable '%s' is a SESSION variable")`

### 7.2 三条必须遵守的枚举规则

| 规则 | 依据 | 后果（不做会怎样） |
|---|---|---|
| **R1** `isGlobal` 时跳过 `sess_global_type == 2` 的变量 | `systemVar.c:1758-1766` 会 `elog(ERROR)` | `SHOW GLOBAL VARIABLES` 整条语句报错 |
| **R2** 无论哪种作用域，都跳过名为 `global_system_vars_init_flag` 的条目 | `systemVar.c:934-944` 塞进去的哨兵（N6） | 输出里多一个不存在的假变量 |
| **R3** session 枚举时，对 `sess_global_type == 1`（global only）的变量，值应取自 `globalSystemVars` 而非 session 哈希表的拷贝 | **［代码核实］** `initSessionSystemVars()`（`systemVar.c:965-982`）**无条件把全部 199 个变量拷进 session 哈希表**，包括 global-only 的；`SET GLOBAL` 之后这份拷贝不会更新 | `SHOW SESSION VARIABLES` 对 global-only 变量返回连接建立时的陈旧快照 |

R3 同时也让行为对齐 MySQL：MySQL 的 `SHOW SESSION VARIABLES` 会列出 global-only 变量并显示其**当前** global 值。

### 7.3 `LOCAL`

**［代码核实］** `mys_gram.y:2583` 与 `2691` 已把 `LOCAL` 映射到 session 视图，与 MySQL 一致。**不需要改。**

### 7.4 视图与 SRF 的作用域参数

两个视图分别固定传 `false`（session）和 `true`（global）：

```sql
CREATE OR REPLACE VIEW mys_informa_schema.empty_session_variables AS
  SELECT variable_name::varchar(128) AS variable_name,
         value::varchar(1024)        AS value
  FROM mysql.show_variables(false);

CREATE OR REPLACE VIEW mys_informa_schema.empty_global_variables AS
  SELECT variable_name::varchar(128) AS variable_name,
         value::varchar(1024)        AS value
  FROM mysql.show_variables(true);
```

⚠ **视图的输出列名必须保持小写 `variable_name` / `value`** —— 因为 `SHOW VARIABLES WHERE Variable_name = 'x'` 这条路径里，用户写的 `Variable_name` 会被 PG 折叠成小写去匹配视图列（`mys_gram.y:2667-2688` 直接把用户 `where_clause` 原样挂上去）。MySQL 风格的 `Variable_name` / `Value` **只在语法层的 target list 里做别名**，与 `SHOW STATUS` 的做法一致（`mys_gram.y:3331-3332`）。

---

## 8. 实施步骤

> 版本基线：**［代码核实］** `contrib/aux_mysql/aux_mysql.control` 当前 `default_version = '1.6'`。本次新增 `1.6 → 1.7` 升级脚本，**不修改任何已发布的历史脚本**。

### 步骤 1 — 内核：override 链提取（`src/backend/adapter/mysql/`）

1. 在 `systemVar.c` 新增 `MysResolveSystemVarOverride(const char *name, bool isSession, bool forShow)`，实现搬自 `mys_uservar.c:137-207`，并按 §6.6 调整为「先规范化名字/作用域，再匹配」。
2. 新增 §6.3 的三项：`version` → `mysql_server_version`；`version_comment` → 从 `version` 分支拆出来，返回目录值（或直接返回 `'MySQL Server (GPL)'`）；`max_allowed_packet` → `psprintf("%d", mysql_max_allowed_packet)`，**删掉 `mys_uservar.c:155` 的静态 `"16777216"`**。
3. `forShow == true` 且变量有 `showResult` 映射时，override 值需再过一遍映射（唯一实际受影响的是 `autocommit`：`1` → `ON`）。
4. 在 `src/include/adapter/mysql/systemVar.h`（现有声明块在 84-96 行）导出该函数。
5. 改 `mys_uservar.c:128` 的 `mys_get_system_variable()`，用调用替换内联 if 链。**此步骤单独可编译、可测**——先验证 `@@` 路径没有回归，再往下做。

### 步骤 2 — 内核：枚举函数

在 `systemVar.c` 新增：

```c
typedef struct MysSystemVarRow
{
    const char *name;
    const char *value;
} MysSystemVarRow;

/* 返回 List<MysSystemVarRow *>，在 CurrentMemoryContext 中分配 */
List *MysEnumerateSystemVariables(bool isGlobal);
```

实现要点：
- 开头照抄现有懒初始化写法：`if (sessionSystemVars == NULL) initSystemVariables();`（现有调用点 `systemVar.c:464` / `610` / `1721`）。
- **名单来自后端本地数组 `baseSystemVars[]` / `baseSystemVarsNum`（`systemVar.c:317-318`），不要 `hash_seq_search` 共享内存哈希表**（原因见 §5 推荐理由 5：自旋锁下禁止 `palloc`）。注意 `dynAddSystemVar()` 也会往这个数组里追加（`systemVar.c:1045`），所以它始终是完整名单。
- 逐个名字：
  1. 按 R1/R2 过滤（`sess_global_type` 从 `baseSystemVars[i]` 直接读，无需加锁）。
  2. `MysResolveSystemVarOverride(name, !isGlobal, /*forShow*/true)`，非 NULL 直接用。
  3. 否则走常规查询拿 `SystemVar *`（session 或 global 哈希表；R3：session 枚举遇到 type==1 时改查 global 表），再 `getConfValue(sv, 2)`。
  4. 结果为 NULL → 回退 `varConfValue`（N5）。仍为 NULL → 空串（SHOW 永不返回 NULL）。
- 每次 global 查询各自短暂持锁，**不要跨整个循环持锁**。
- 顺手修 `getConfValue()`（`systemVar.c:1786-1832`）：两个分支的循环结束后若 `ret == NULL`，`ret = pstrdup(systemVar->varConfValue)`。
- 在 `systemVar.h` 导出。

### 步骤 3 — `mysm`：SRF 包装

在 `src/backend/utils/ddsm/mysm/systemVar.c`（已有 `getSystemVariable` / `setSystemVariable`）新增：

```c
PG_FUNCTION_INFO_V1(showSystemVariables);
Datum showSystemVariables(PG_FUNCTION_ARGS);
```

materialize 模式的完整写法直接照抄 `contrib/aux_mysql/src/mysql_protocol.c:1725-1783`（`ReturnSetInfo` 校验 → `get_call_result_type` → 切到 `ecxt_per_query_memory` → `tuplestore_begin_heap` → 循环 `tuplestore_putvalues` → `tuplestore_donestoring`）。两列均为 `text`。

`mysm` 的 meson 目标已存在，无需改构建；新增的内核符号会被 `src/tools/check_mysql_kernel_exports.sh` 的 G3 门禁自动覆盖（`contrib/aux_mysql/meson.build:26-29`）。

### 步骤 4 — 扩展脚本 `aux_mysql--1.6--1.7.sql`（新建）

```sql
/* aux_mysql 1.6 -> 1.7
 * P2-15: SHOW VARIABLES 此前对所有变量返回空串（empty_* 视图硬编码 '' ）。
 * 改为经 mysql.show_variables() 读取会话/全局系统变量的当前值。
 * 视图名与列定义保持不变，以便新旧内核与新旧扩展任意组合都不会丢功能。
 */

CREATE OR REPLACE FUNCTION mysql.show_variables(pg_catalog.bool)
RETURNS TABLE(variable_name pg_catalog.text, value pg_catalog.text)
AS '$libdir/mysm', 'showSystemVariables'
LANGUAGE C VOLATILE STRICT;
GRANT EXECUTE ON FUNCTION mysql.show_variables(pg_catalog.bool) TO PUBLIC;

CREATE OR REPLACE VIEW mys_informa_schema.empty_session_variables AS
  SELECT variable_name::varchar(128) AS variable_name,
         value::varchar(1024)        AS value
  FROM mysql.show_variables(false);

CREATE OR REPLACE VIEW mys_informa_schema.empty_global_variables AS
  SELECT variable_name::varchar(128) AS variable_name,
         value::varchar(1024)        AS value
  FROM mysql.show_variables(true);

GRANT SELECT ON mys_informa_schema.empty_session_variables TO PUBLIC;
GRANT SELECT ON mys_informa_schema.empty_global_variables  TO PUBLIC;

/* N1: version 从来没进过目录表 */
INSERT INTO mys_informa_schema.base_variables
VALUES ('version', '8.4.10', '8.4.10', 0, true, false, null, null, null)
ON CONFLICT (variable_name) DO NOTHING;

/* N7: 与内核 pg_proc.dat 的 provolatile='v' 对齐 */
CREATE OR REPLACE FUNCTION mysql.get_system_variable(pg_catalog.text, pg_catalog.bool)
RETURNS pg_catalog.text
AS '$libdir/mysm', 'getSystemVariable'
LANGUAGE C STRICT VOLATILE;
```

配套：
- `contrib/aux_mysql/aux_mysql.control`：`default_version = '1.7'`
- `contrib/aux_mysql/meson.build` 的 `install_data(...)` 列表（41-48 行）加 `'aux_mysql--1.6--1.7.sql'`
- `INSERT ... ON CONFLICT` 依赖 `base_variables` 上的 `primary key(variable_name)`（**［代码核实］** `aux_mysql--1.3--1.4.sql:3331`），成立

### 步骤 5 — 语法层 `mys_gram.y`

1. 在静态函数声明区（237-246 行附近，紧邻 `mysqlMakeShowDatabases`）加：
   ```c
   static Node *mysqlMakeShowVariables(bool isGlobal, const char *likePattern,
                                       Node *whereClause, core_yyscan_t yyscanner);
   ```
2. 在 `mysqlMakeShowDatabases()`（252-272 行）后面实现它：
   - `targetList`：`createResTargetWithColumn("Variable_name", "variable_name", yyscanner)` + `createResTargetWithColumn("Value", "value", yyscanner)`（N2）
   - `fromClause`：`createRangeVar("mys_informa_schema", isGlobal ? "empty_global_variables" : "empty_session_variables")`
   - `whereClause`：`likePattern != NULL` 时构造现有的 `makeSimpleA_Expr(AEXPR_ILIKE, "~~*", …)`（保持现状，不要改成 `~~`，那是独立的大小写敏感性问题）；否则透传传入的 `whereClause`
   - `sortClause`：`ORDER BY 1`，照抄 `mys_gram.y:3346-3350` 的 `SortBy` 构造（N3）
3. 把 8 条规则（2555 / 2583 / 2611 / 2639 / 2667 / 2691 / 2715 / 2739）的规则体各自替换为一行：
   ```c
   $$ = mysqlMakeShowVariables(/*isGlobal*/ false, $4, NULL, yyscanner);   /* 各规则的 $n 下标不同 */
   ```
   **规则头（`SHOW VARIABLES LIKE SCONST` 等）一个字都不要动**——不新增非终结符，零 LALR 冲突风险。
4. `bison` 重新生成后确认 conflict 数量与改动前**完全一致**（这是本步骤的验收条件）。

### 步骤 6 — 构建与安装

`meson compile && meson install`；确认 `inst/share/extension/aux_mysql--1.6--1.7.sql` 已就位（**［代码核实］** 该目录是已安装树，`inst/share/extension/aux_mysql--1.5--1.6.sql` 等已在其中）。已有数据库需执行 `ALTER EXTENSION aux_mysql UPDATE TO '1.7';`。

### 步骤 7 — 测试（见 §10）

---

## 9. 明确不在范围内

以下问题在本次分析中被确认存在，但各自需要独立设计，混进来会让本次改动无法干净验证：

| # | 问题 | 为什么单列 |
|---|---|---|
| A | **`sql_mode` 的 `STRICT_TRANS_TABLES` 读写脱节** | **［代码核实］** `systemVar.c:1643-1647` 把解析结果硬写成 `false`（原赋值被注释掉）。`SHOW` 显示严格模式开着，实际没开。改显示值会改变客户端的行为判断（很多 ORM 依据 `sql_mode` 决定是否自己做校验），必须配套决定「到底要不要真的实现严格模式」——那是独立课题 |
| B | **`port` / `datadir` / `basedir` / `hostname` / `socket` 等环境类只读变量缺失或撒谎** | **［代码核实］** 前四个在 `base_variables` 里根本不存在；`socket` 存在但硬编码 `'/tmp/mysql.sock'`（3416 行附近）。补齐它们是「只读环境变量对齐」任务，与「SHOW 读不到值」这个 bug 无关，且需要逐个确认真实来源 |
| C | **`character_set_*` / `collation_*` 与真实库编码的关系** | 目录值、`@@` override 静态值都是 `utf8mb4` / `utf8mb4_general_ci`，与库的真实 `server_encoding` / `lc_collate` 无关联。牵涉 COLLATION_CLUSTER_SPEC.md 的整体排序规则设计，不能在这里顺手改 |
| D | **`SET GLOBAL` 的持久化与跨会话可见性** | 与 P2-10 规格 §5 项 B 同源：全局写路径本身没打通。本次只保证「已经写进 `globalSystemVars` 的值能被 `SHOW GLOBAL VARIABLES` 读出来」，不扩大全局写的能力范围 |
| E | **`SHOW STATUS` 的内容语义** | **［代码核实］** `mys_gram.y:3322` 把它映射到 `mys_informa_schema.db_status`，而该视图是 `pg_settings` 的直接投影（`aux_mysql--1.4--1.5.sql:26-30`）——返回的是 PG GUC，不是 MySQL 状态变量。同时 `SHOW GLOBAL STATUS` 与 `SHOW SESSION STATUS` 返回完全相同的内容（`show_session_global_opt` 是无值规则，作用域被丢弃）。这是另一个独立缺口 |
| F | **`SHOW VARIABLES LIKE` 的大小写敏感性** | 现在用 `AEXPR_ILIKE` / `~~*`（8 条规则一致）。MySQL 的行为取决于 `variable_name` 列的排序规则，通常也是大小写不敏感，所以**保持现状**。但这与 `4553c8dac6`（LIKE 不再映射 ILIKE）的方向不一致，值得单独复核 |
| G | **把 8 条语法规则合并成 `SHOW show_variables_scope_opt VARIABLES …`** | 收益是可读性，风险是 LALR(1) 冲突（见 §5 理由 5）。本次用 helper 函数拿到了 95% 的去重收益，规则结构留给单独的语法清理任务 |
| H | **视图改名（`empty_session_variables` → `session_variables`）** | 名字修好后就成了谎话，但改名会破坏「新内核 + 旧扩展」的降级路径（§5 理由 2）。建议等确认所有部署都升到 1.7 之后再单独做一次改名 + 保留旧名为兼容视图 |

---

## 10. 验证方案

### 10.1 修复前基线（必须先跑，留证据）

通过 MySQL 监听端口连接后执行，记录输出：

```sql
SHOW VARIABLES LIKE 'version%';        -- 预期基线：1 行，version_comment / 空串
SHOW VARIABLES LIKE 'autocommit';      -- 预期基线：1 行，值为空串
SHOW GLOBAL VARIABLES LIKE 'sql_mode'; -- 预期基线：1 行，值为空串
SELECT COUNT(*) FROM (SELECT 1) x;     -- 连接可用性对照
```

### 10.2 修复后功能验证

| # | 用例 | 期望 |
|---|---|---|
| V1 | `SHOW VARIABLES LIKE 'version%'` | ≥2 行（`version` = `8.4.10-openhalo-1.0`，`version_comment` = `MySQL Server (GPL)`），**无空串** |
| V2 | `SHOW VARIABLES LIKE 'autocommit'` | 值为 `ON`（**不是 `1`**）——证明走的是 `showResult` 映射而非 SELECT 映射 |
| V3 | `SET autocommit = 0;` 然后 `SHOW VARIABLES LIKE 'autocommit'` | 值变为 `OFF` ——证明读的是会话当前状态，不是启动种子值 |
| V4 | `SHOW VARIABLES LIKE 'transaction_isolation'` 与 `SELECT @@transaction_isolation` | **两者完全一致**，且与 `SELECT current_setting('default_transaction_isolation')` 语义一致（P2-10 不倒退） |
| V5 | `SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE;` 后重跑 V4 | 两者同步变为 `SERIALIZABLE` |
| V6 | `SHOW VARIABLES LIKE 'max_allowed_packet'` / `SELECT @@max_allowed_packet` / `current_setting('mysql_max_allowed_packet')` | 三者数值一致（N4 收敛） |
| V7 | `SHOW GLOBAL VARIABLES` （裸，无过滤） | **不报错**、返回全部有 global 作用域的变量（R1 生效） |
| V8 | `SELECT COUNT(*) FROM (SHOW GLOBAL VARIABLES) …` 或直接查视图：`SELECT * FROM mys_informa_schema.empty_global_variables WHERE variable_name = 'global_system_vars_init_flag'` | 0 行（R2 生效） |
| V9 | `SHOW VARIABLES` 裸执行 | 行数 = 200（199 + 补的 `version`）；**无任何 value 为 NULL 或空串**（可用 `SHOW VARIABLES WHERE Value = ''` 断言 0 行） |
| V10 | `SHOW VARIABLES WHERE Variable_name = 'sql_mode'` | 1 行有值 —— 证明 WHERE 路径的小写列名匹配没被别名破坏（§7.4） |
| V11 | `SHOW LOCAL VARIABLES LIKE 'autocommit'` 与 `SHOW SESSION VARIABLES LIKE 'autocommit'` | 结果相同 |
| V12 | 结果集列名 | `Variable_name` / `Value`（N2）；行按变量名升序（N3） |
| V13 | 双会话：A 执行 `SET GLOBAL <某 global 变量> = …`，B 执行 `SHOW GLOBAL VARIABLES LIKE '<该变量>'` | B 看到 A 写入的值（证明读的是共享内存哈希表，不是本地拷贝） |
| V14 | `SHOW SESSION VARIABLES LIKE '<某 global-only 变量>'`，在另一会话 `SET GLOBAL` 之后 | 看到新值（R3 生效） |

### 10.3 跨方言隔离验证（**必做**）

改动触及三个层次，必须逐层证明 PG / TDS 不受影响：

| 层 | 改了什么 | 隔离论证 | 验证动作 |
|---|---|---|---|
| 语法层 | `mys_gram.y` 的 8 条规则 | **［代码核实］** 该文件是 MySQL 方言专属语法，PG 走 `src/backend/parser/gram.y`，T-SQL 走 Babelfish 的 `gram-tsql-rule.y`，三者独立编译、按 `sql_dialect` / 兼容模式分派（`src/include/parser/parser.h:76`） | PG 连接执行 `SHOW VARIABLES` → 应仍是 PG 原有行为（语法错误），**不能**变成查视图 |
| 内核 C | `systemVar.c` 新增函数 + 修 `getConfValue()` NULL 回退 + `mys_uservar.c` 改调用 | 这两个文件里的所有函数只被 MySQL 路径调用；`getConfValue()` 是 `static`，调用方只有 `getSystemVariableValueImpl()` | `nm` / `grep` 确认无新的跨方言调用点；跑全量回归 |
| 扩展 SQL | `aux_mysql` 1.6→1.7 | `aux_mysql` 只在 MySQL 兼容库里安装；PG 库与 Babelfish 库不含该扩展 | 在纯 PG 库执行 `\dx` 确认无 `aux_mysql`；TDS 连接跑一遍 T-SQL 冒烟 |

具体执行：

1. **PG**：`meson test` 的 `regress/regress` 套件必须仍是 **232/232 全绿**（FUSION_PLAN.md §5.9 记录的基线）。
2. **PG 手工**：psql 连接执行 `SHOW default_transaction_isolation` / `SHOW timezone` / `SHOW ALL` —— 输出与改动前逐字一致。
3. **MySQL**：`meson test` 的 `aux_mysql` 套件（`005_mysql_compat` / `006_pg_dump_restore` / `007_mysql_parallel`）全绿。
4. **TDS**：真实 TDS 连接（sqlcmd 或等价客户端）执行一段 T-SQL 冒烟（建表 / 插入 / `SELECT @@VERSION` / `sys.dm_exec_sessions` 查隔离级别），与改动前一致。
5. **四套件汇总**：沿用 P2-16 的验收口径 —— `meson test` 四套件 13/13。

### 10.4 回归测试落地

**［代码核实］** MySQL 兼容套件位置：`contrib/aux_mysql/t/mysql_compat/`，由 `contrib/aux_mysql/meson.build:59-62` 的 `t/005_mysql_compat.pl` 驱动，脚本 `run_mysql_compat.sh` 用真实 `mysql` 客户端跑 `.sql` 并与 `expected/*.out` 逐字对比。

在 `00_session.sql` 现有变量测试段（40-47 行，`system_variable_bare` 附近）之后追加，沿用同样的 `test_name` / `passed` 断言风格以避免受值本身波动影响：

```sql
-- P2-15: SHOW VARIABLES 必须返回真实值
SELECT 'show_variables_not_empty' AS test_name,
       COUNT(*) = 0 AS passed
FROM (SHOW VARIABLES) v WHERE v.Value = '' OR v.Value IS NULL;   -- 若语法不支持子查询包裹，改用下一条形式
```

若 `SHOW` 不能出现在子查询里（**［推断］**，需实测），改为直接查视图 —— 这同时也验证了视图层：

```sql
SELECT 'show_variables_not_empty' AS test_name,
       COUNT(*) = 0 AS passed
FROM mys_informa_schema.empty_session_variables
WHERE value = '' OR value IS NULL;

SELECT 'show_variables_version' AS test_name,
       value LIKE '8.%' AS passed
FROM mys_informa_schema.empty_session_variables WHERE variable_name = 'version';

SELECT 'show_variables_autocommit_show_mapping' AS test_name,
       value IN ('ON','OFF') AS passed
FROM mys_informa_schema.empty_session_variables WHERE variable_name = 'autocommit';

SELECT 'show_variables_matches_select_var' AS test_name,
       (SELECT value FROM mys_informa_schema.empty_session_variables
        WHERE variable_name = 'transaction_isolation') = @@transaction_isolation AS passed;

SELECT 'show_global_variables_no_sentinel' AS test_name,
       COUNT(*) = 0 AS passed
FROM mys_informa_schema.empty_global_variables
WHERE variable_name = 'global_system_vars_init_flag';
```

外加至少一条真正走 `SHOW` 语法的用例（验证语法层的别名与排序）：

```sql
SHOW VARIABLES LIKE 'version%';
SHOW GLOBAL VARIABLES LIKE 'autocommit';
```

这两条的输出进 `expected/00_session.out`，用于锁定列名 `Variable_name` / `Value` 与行序。**注意**：`version` 的值来自 GUC `mysql_server_version`，改版本号会让 expected 失配 —— 若嫌脆弱，就只保留断言式用例，把 `SHOW` 语法用例的 LIKE 换成 `autocommit` 这种稳定值。

---

## 11. 工作量与风险评估

### 11.1 工作量

| 步骤 | 内容 | 估时 |
|---|---|---|
| 1 | override 链提取 + 三项新增（内核 C） | 3 h |
| 2 | 枚举函数 + `getConfValue()` NULL 回退（内核 C） | 4 h |
| 3 | `mysm` SRF（照抄现成范式） | 1.5 h |
| 4 | 扩展 1.6→1.7 脚本 + control + meson | 1 h |
| 5 | 语法层 helper + 8 条规则改写 + bison 验证 | 3 h |
| 6 | 构建、安装、手工验证 V1–V14 | 3 h |
| 7 | 回归用例 + expected + 四套件跑通 | 4 h |
| 8 | 跨方言隔离验证（PG 232/232 + TDS 冒烟） | 2 h |
| | **合计** | **约 21.5 h（1.5～2.5 人日）** |

### 11.2 风险

| 风险 | 等级 | 说明与缓解 |
|---|---|---|
| **「从空变有值」本身触发客户端新分支** | **中（最大风险）** | 很多驱动（Connector/J、ORM）在连接握手时读 `SHOW VARIABLES`，此前拿到空串走的是「都用默认值」的分支；现在拿到真值可能走进从未测过的分支（例如按 `sql_mode` 决定是否本地校验、按 `lower_case_table_names` 决定标识符处理）。**缓解**：修复后必须用真实客户端（`mysql` CLI + 至少一个驱动）做端到端连接测试，不能只跑 SQL 断言 |
| 语法层 LALR 冲突 | **低** | 方案刻意不动规则头。验收条件明确：bison 冲突数与改动前完全一致 |
| 自旋锁下 `palloc` 导致 panic | **低（但后果严重）** | 已在 §5/§8 明确要求走 `baseSystemVars[]` 本地名单 + 短临界区。**代码评审时必须专门检查这一点** |
| 扩展版本偏斜（内核新、扩展旧） | **低** | 视图名与列定义不变 → 旧扩展下退化为今天的空值行为，不会报错 |
| `getConfValue()` NULL 回退改动波及 `@@` 路径 | **低** | 该改动只在「有映射且没匹配上」这条此前返回 NULL 的分支生效，属于纯修复；`@@` 路径此前返回 SQL NULL 是明显的 bug |
| `version` 插入与既有部署冲突 | **极低** | `ON CONFLICT (variable_name) DO NOTHING`，主键已存在（`aux_mysql--1.3--1.4.sql:3331`） |
| 波及 PG / TDS | **极低** | 三层改动全部落在 MySQL 专属文件（`mys_gram.y` / `adapter/mysql/` / `commands/mysql/` / `mysm` / `aux_mysql` 扩展），无共享代码路径。仍按 §10.3 逐层验证 |

### 11.3 建议的实施顺序（可分阶段合入）

1. **阶段 1**（步骤 1）：override 提取 + `@@` 路径不回归。可独立提交，风险最低。
2. **阶段 2**（步骤 2+3+4）：枚举函数 + SRF + 视图。此时 `SELECT * FROM mys_informa_schema.empty_session_variables` 已能返回真值，`SHOW VARIABLES` 也已经有值（只是列名和排序还是旧的）。**这一步就已经解决了 P2-15 的核心症状。**
3. **阶段 3**（步骤 5）：语法层列名 + 排序（N2/N3）。纯打磨，可独立评估。

若时间紧张，阶段 1+2 即可关闭 P2-15 主体，阶段 3 单独跟进。

---

## 附：本文档引用的关键位置索引

| 位置 | 内容 |
|---|---|
| `src/backend/parser/mysql/mys_gram.y:2555,2583,2611,2639,2667,2691,2715,2739` | 8 条 `SHOW VARIABLES` 规则 |
| `src/backend/parser/mysql/mys_gram.y:2864-2880` | `SHOW WARNINGS` —— 语法层调 SRF 的先例 |
| `src/backend/parser/mysql/mys_gram.y:3322-3352` | `SHOW STATUS` —— 列别名 + ORDER BY 的写法范本 |
| `src/backend/parser/mysql/mys_gram.y:252-272` | `mysqlMakeShowDatabases()` —— helper 函数放置位置 |
| `contrib/aux_mysql/aux_mysql--1.3--1.4.sql:3319-3332` | `base_variables` 表定义 |
| `contrib/aux_mysql/aux_mysql--1.3--1.4.sql:3531-3543` | 两个空视图 |
| `contrib/aux_mysql/aux_mysql--1.3--1.4.sql:3641-3644` | `mysql.get_system_variable()`（IMMUTABLE，应为 VOLATILE） |
| `contrib/aux_mysql/aux_mysql--1.5--1.6.sql:160-163` | `mysql.show_warnings()` SQL 声明范本 |
| `contrib/aux_mysql/src/mysql_protocol.c:1725-1783` | materialize SRF 实现范本 |
| `src/backend/adapter/mysql/systemVar.c:301-315` | `SystemVar` 结构体 |
| `src/backend/adapter/mysql/systemVar.c:405-455` | `ForSelect` / `ForShow` |
| `src/backend/adapter/mysql/systemVar.c:625-640` | `initSystemVariables()` |
| `src/backend/adapter/mysql/systemVar.c:934-944` | 全局哨兵条目 `global_system_vars_init_flag` |
| `src/backend/adapter/mysql/systemVar.c:965-982` | `initSessionSystemVars()`（无条件全量拷贝） |
| `src/backend/adapter/mysql/systemVar.c:1006-1054` | `dynAddSystemVar()` |
| `src/backend/adapter/mysql/systemVar.c:1059-1082` | `lockGlobalSystemVars()`（**自旋锁**） |
| `src/backend/adapter/mysql/systemVar.c:1558-1697` | `applySystemVarValue()`（只有 4 个变量有真实副作用） |
| `src/backend/adapter/mysql/systemVar.c:1710-1782` | `getSystemVariableValueImpl()`（作用域分流与报错） |
| `src/backend/adapter/mysql/systemVar.c:1786-1832` | `getConfValue()`（`selectShowType` 语义 + NULL 漏洞） |
| `src/include/adapter/mysql/systemVar.h:84-96` | 导出声明块 |
| `src/backend/commands/mysql/mys_uservar.c:128-230` | `mys_get_system_variable()` + override 链 |
| `src/include/catalog/pg_proc.dat:1641-1643` | `mys_get_system_variable` 目录声明（VOLATILE） |
| `src/backend/utils/misc/guc_tables.c:581,584,585` | `mysql_server_version` / `mysql_port` / `mysql_max_allowed_packet` |
| `src/backend/utils/ddsm/mysm/systemVar.c:55-73` | `getSystemVariable` SQL 函数实现（新 SRF 的邻居） |
| `contrib/aux_mysql/meson.build:41-48,59-62` | 扩展脚本安装列表 / TAP 测试列表 |
| `contrib/aux_mysql/t/mysql_compat/00_session.sql:40-47` | 现有变量测试段（新用例插入点） |
