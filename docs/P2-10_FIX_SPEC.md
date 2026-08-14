# P2-10 实施规格:MySQL 事务隔离级别设置失效

> ✅ **已实施完成(2026-08-14,Sonnet 5 + high)**。按本规格第 4 节四个步骤实施,第 6 节验证全部通过(含四套件 13/13),第 7 节文档更新已完成。本文档保留作为分析过程记录。
>
> 本文档不纳入版本控制,仅作本地工作记录。
> 分析完成时间:2026-08-14(Opus 5 + max reasoning 分析,交由 Sonnet 实施)
> 关联:[FUSION_PLAN.md](FUSION_PLAN.md) §6 P2-10

## 0. 给实施者的前置说明

**这份规格里的每一条结论都已经在代码里核实过,不要凭直觉改动。** 特别注意第 2 节"上一次失败的尝试",避免重复踩坑。

实施范围严格限定在第 4 节。第 5 节列的问题**不要顺手一起修**——它们是独立的、更大的缺口,混在一起会让这次改动无法验证也无法回退。

---

## 1. 根因:三层独立缺陷

`SET SESSION TRANSACTION ISOLATION LEVEL x` 静默失效,不是单一 bug,是三层各自独立的问题叠加。三层都必须处理,只修一层不会产生任何可见改善。

### 第 1 层:写路径根本没接线(主症状)

语法层 `src/backend/parser/mysql/mys_gram.y:1802-1834` 把三种写法都转成自定义 GUC 名:

| 语句 | 生成的 `VariableSetStmt.name` |
|---|---|
| `SET TRANSACTION ...` | `mysql._set_transaction` |
| `SET SESSION TRANSACTION ...` | `mysql._set_session_transaction` |
| `SET GLOBAL TRANSACTION ...` | `mysql._set_global_transaction` |

这些名字进入 `contrib/aux_mysql/src/mys_utility.c:636-651` 的分发:凡是 `mysql._` 前缀的都交给 `MysExecSetVariableStmt()`(第 2181 行)。

**该函数末尾(第 2344-2350 行)是问题所在**:

```c
        (void) MysApplyAutocommitAssignment(assignment);
        (void) MysApplySqlModeAssignment(assignment);
        (void) MysApplyTimeZoneAssignment(assignment);
        (void) MysApplyGlobalTimeZoneAssignment(assignment);
    }

    /* Other driver initialization variables retain the established no-op. */
```

只有 `autocommit` / `sql_mode` / `time_zone` / `global time_zone` **四个变量真正接线**,其余一律静默 no-op。这个 no-op 兜底本身是**有意设计**(MySQL 驱动握手时会发一堆 `SET`,不能报错),不要改掉它;要做的是给 `transaction_isolation` 补一条真正的处理分支。

> ⚠️ 我第一次排查时 `grep "_set_batch"` 什么也没搜到,误判成"完全没有处理代码"。实际分发是**按 `"mysql._"` 前缀匹配**再比后缀的,直接搜完整变量名会漏掉。

### 第 2 层:格式不匹配(修好第 1 层后立刻会撞上)

`mys_gram.y:2393-2396` 的 `iso_level` 产生的是 **MySQL 展示格式**:

```
READ UNCOMMITTED  →  "READ-UNCOMMITTED"
READ COMMITTED    →  "READ-COMMITTED"
REPEATABLE READ   →  "REPEATABLE-READ"
SERIALIZABLE      →  "SERIALIZABLE"
```

而 PG 的 `transaction_isolation` / `default_transaction_isolation` GUC 只接受 `read uncommitted` / `read committed` / `repeatable read` / `serializable`(小写空格分隔,已用 psql 实测确认)。

**这一层有一个已经在生产路径上发作的既有 bug**,与 `SET` 无关:

`mys_utility.c:395-398`(处理 `START TRANSACTION` / `BEGIN`):

```c
if (strcmp(item->defname, "transaction_isolation") == 0)
    SetPGVariable("transaction_isolation", list_make1(item->arg), true);
```

`item->arg` 里是 MySQL 格式串,直接塞给 PG 必然被拒。**实测确认**:

```
mysql> START TRANSACTION ISOLATION LEVEL READ COMMITTED;
ERROR 1210 (HY000): invalid value for parameter "transaction_isolation": "READ-COMMITTED"
```

也就是说 `START TRANSACTION ISOLATION LEVEL x` 这条**早就是硬报错**,不是本次改动引入的,必须一并修掉。

**`iso_level` 只有一个消费点**(`mys_gram.y:13862` 的 `transaction_mode_item`),已 grep 确认。因此有两种改法:改 `iso_level` 让它直接产出 PG 格式,或保持 MySQL 格式、在写入前转换。**必须选后者**——理由见第 3 节。

### 第 3 层:读路径与真实状态完全脱节

`@@transaction_isolation` 的解析链:

```
mys_gram.y:19911  @@name
  → pg_catalog.mys_get_system_variable('transaction_isolation', true)
  → src/backend/commands/mysql/mys_uservar.c:99
  → (不在静态快速返回表里)
  → getSystemVariableValueForSelect()
  → src/backend/adapter/mysql/systemVar.c 的会话/全局哈希表
  → varConfValue,初值来自 aux_mysql--1.3--1.4.sql:3420 的目录表:
```

```sql
insert into mys_informa_schema.base_variables
values('transaction_isolation', 'REPEATABLE-READ', 'REPEATABLE-READ', 0, true, true, null, null, null);
```

**这个存储与 PG 真实的隔离级别毫无关联。** 后果是当前系统在主动误导客户端:

| | MySQL 客户端看到的 | 实际生效的 |
|---|---|---|
| 默认隔离级别 | `REPEATABLE-READ` | `read committed`(PG 默认,已实测) |

应用如果检查隔离级别会得到错误答案;如果依赖 REPEATABLE READ 语义,会在毫不知情的情况下拿到更弱的保证。**这是本问题里危害最大的一层**,比"SET 不生效"更严重。

**已有先例可循**:同一个函数里 `time_zone` 就是特例化处理的(`mys_uservar.c:144-157`),绕开目录表直接读真实会话状态。照抄这个模式即可。

**已确认测试套件对 `transaction_isolation` 零断言**(`grep -rn "transaction_isolation\|REPEATABLE-READ" contrib/aux_mysql/t/` 无结果),所以让读路径说真话不会破坏现有回归。

---

## 2. 上一次失败的尝试(不要重复)

Sonnet 先前尝试过:把 `mys_gram.y:1802-1823` 的两条规则从 `VAR_SET_VALUE` + 假 GUC 名改成 PG 原生的 `VAR_SET_MULTI` + `name="TRANSACTION"` / `"SESSION CHARACTERISTICS"`。

结果:语句确实触达了真实代码路径,但立刻撞上第 2 层格式问题,报 `invalid value for parameter "default_transaction_isolation": "READ-COMMITTED"`。**从"静默无效"变成"报错",不是净改善**,已 `git checkout` 回退。

**这次不要走语法层改造路线**,原因:

1. 改语法要重新生成 bison,有引入语法冲突的风险(`mys_gram.y` 有 `%expect` 计数约束)。
2. `mys_gram.y:2234` 那套 PG 风格的 `set_rest` 规则**在普通 `SET` 语句下不可达**——`VariableSetStmt` 里引用它的三行(2214/2220/2226)全部被注释掉,`set_rest` 仅通过 `SetResetClause`(`ALTER ... SET` 场景)存活。顺带一提,该规则 2243-2244 行有一处明显的复制粘贴 bug(`SESSION CHARACTERISTICS AS TRANSACTION transaction_mode_list` 连写两遍),因为不可达所以一直没暴露——**本次不要动它**,记录在第 5 节。
3. 全部改动放在 `contrib/aux_mysql`(扩展)+ 一处内核读路径,不碰语法,风险最低、可独立回退。

---

## 3. 设计决策与理由

### 决策 1:保持 `iso_level` 输出 MySQL 格式,在写入 PG 前转换

因为读路径(`@@transaction_isolation`)、目录表 `base_variables`、以及 `SystemVar` 的 `selectResult`/`showResult` 展示机制**全都以 MySQL 格式为准**。改 `iso_level` 会让这些地方全部错位。转换点放在"即将写入 PG GUC"的位置,是唯一的格式边界。

### 决策 2:PG GUC 作为唯一事实来源,读路径做格式翻译

两种可选架构:

- **A**:MySQL 变量存储为准,写入时同步刷 PG GUC → 两个事实来源,会漂移。
- **B**:PG GUC 为准,读路径翻译 → 单一事实来源。**采用 B。**

B 的额外好处:与已有的 `time_zone` 先例一致;并且即使隔离级别是被其他途径改变的(例如 `START TRANSACTION ISOLATION LEVEL`),`@@transaction_isolation` 也能如实反映。

### 决策 3:`SET SESSION TRANSACTION` 映射到 `default_transaction_isolation`

MySQL 语义核对(与 PG 语义的对应关系):

| MySQL 写法 | MySQL 语义 | PG 对应 |
|---|---|---|
| `SET SESSION TRANSACTION ISOLATION LEVEL x` | 改会话默认,影响后续所有事务 | `default_transaction_isolation`(会话级 GUC)✅ 干净对应 |
| `SET TRANSACTION ISOLATION LEVEL x`(裸写) | **仅下一个事务** | PG 的 `SET TRANSACTION` 是"当前事务",语义不同 ❌ |
| `SET GLOBAL TRANSACTION ISOLATION LEVEL x` | 改全局默认,影响新会话 | 需要 shmem 全局变量 + 新会话初始化钩子 ❌ |

另外核对过:MySQL 里 `@@transaction_isolation` 反映的是**会话变量**,裸 `SET TRANSACTION`(下一事务)**不会**改变它。所以读路径映射到 `default_transaction_isolation` 而不是 `transaction_isolation`,是 MySQL-正确的。

因此本次**只实现 SESSION 作用域**,裸写和 GLOBAL 留作后续(第 5 节),这是有意的范围控制,不是遗漏。

---

## 4. 实施步骤(本次范围)

### 步骤 1:格式转换 —— MySQL → PG

在 `contrib/aux_mysql/src/mys_utility.c` 加一个 static 辅助函数:

```c
/*
 * MySQL's iso_level grammar rule (mys_gram.y) yields MySQL display format
 * ("READ-COMMITTED"); PostgreSQL's transaction_isolation /
 * default_transaction_isolation GUCs only accept "read committed" style.
 * Convert at the boundary rather than changing iso_level itself, because the
 * MySQL-format string is what @@transaction_isolation, the base_variables
 * catalog and the SystemVar display mapping all expect.
 *
 * Returns NULL when the value is not a recognized isolation level, so the
 * caller can fall through to PostgreSQL's own error reporting instead of
 * silently swallowing a typo.
 */
static const char *
MysIsoLevelToPg(const char *mysval)
```

映射(大小写不敏感比较,用 `pg_strcasecmp`):

| 输入 | 输出 |
|---|---|
| `READ-UNCOMMITTED` | `read uncommitted` |
| `READ-COMMITTED` | `read committed` |
| `REPEATABLE-READ` | `repeatable read` |
| `SERIALIZABLE` | `serializable` |
| 其他 | `NULL` |

### 步骤 2:修 `START TRANSACTION` / `BEGIN` 的既有硬报错

`mys_utility.c` 约 395 行,现状:

```c
if (strcmp(item->defname, "transaction_isolation") == 0)
    SetPGVariable("transaction_isolation", list_make1(item->arg), true);
```

改为:先从 `item->arg`(是一个 `A_Const`,`val.sval.sval` 里放着 MySQL 格式串)取出字符串,经 `MysIsoLevelToPg()` 转换,再用**新建的** `A_Const` 传给 `SetPGVariable`。

**不要原地改写 `item->arg` 的内容**——parse tree 可能被缓存复用,就地修改会污染后续执行。

PG18 的 `A_Const` 结构(`src/include/nodes/parsenodes.h:382`):

```c
typedef struct A_Const
{
    NodeTag     type;
    union ValUnion val;
    bool        isnull;
    ParseLoc    location;
} A_Const;
```

构造字符串常量的写法:

```c
A_Const *con = makeNode(A_Const);
con->val.node.type = T_String;
con->val.sval.sval = pstrdup(pgval);
con->isnull = false;
con->location = -1;
```

转换失败(返回 NULL)时**保持原值传下去**,让 PG 自己报错——不要自己编错误信息,PG 的报错已经足够清楚。

### 步骤 3:给 `SET SESSION TRANSACTION` 接线

在 `MysExecSetVariableStmt()`(`mys_utility.c:2181`)**函数开头、进入 `foreach(lc, n->args)` 循环之前**插入分支:

```c
if (n->name != NULL &&
    strcmp(n->name, "mysql._set_session_transaction") == 0)
{
    /* iterate n->args (List of DefElem) and apply, then return */
}
```

`n->args` 的元素形状(来自 `mys_gram.y:13861-13877` 的 `transaction_mode_item`)是 `DefElem`:

| `defname` | `arg` 内容 | 映射到的 PG GUC |
|---|---|---|
| `transaction_isolation` | A_Const 字符串,MySQL 格式 | `default_transaction_isolation` |
| `transaction_read_only` | A_Const 整数(true/false) | `default_transaction_read_only` |
| `transaction_deferrable` | A_Const 整数 | 跳过(MySQL 无此概念) |

用 `SetPGVariable(<pg guc name>, list_make1(<新建 A_Const>), false)`,注意 `is_local` 传 **false**(会话级,不是仅当前事务)。

处理完直接 `return`,不要落到后面的用户变量循环。

`mysql._set_transaction`(裸写)和 `mysql._set_global_transaction` 本次**不接线**,维持现有 no-op,但要在代码里补注释说明是有意为之并指向本文档,避免下一个人以为是漏了。

### 步骤 4:读路径说真话

`src/backend/commands/mysql/mys_uservar.c` 的 `mys_get_system_variable()`(第 99 行)。

在 `time_zone` 特例块(第 144-157 行)**之后**、通用 `getSystemVariableValueForSelect()` 调用(第 176 行)**之前**,加入:

```c
/*
 * transaction_isolation resolves through the real GUC rather than the
 * extension catalog: base_variables seeds a fixed 'REPEATABLE-READ' that
 * has no connection to the backend's actual isolation level, which made
 * @@transaction_isolation actively misleading (it claimed REPEATABLE-READ
 * while the cluster ran at read committed).  Mirrors the time_zone
 * handling above.  MySQL's @@transaction_isolation reports the *session*
 * variable, not the current transaction's override, so this maps to
 * default_transaction_isolation.
 */
```

- 匹配 `transaction_isolation` / `session.transaction_isolation` / `local.transaction_isolation`
- 用 `GetConfigOption("default_transaction_isolation", false, false)` 取值
  (签名见 `src/include/utils/guc.h:411`;`utils/guc.h` 已在该文件第 24 行 include,无需新增)
- 经 PG → MySQL 的**反向**格式转换后返回

反向转换表(在 `mys_uservar.c` 里另写一个 static 辅助函数,与步骤 1 的正向函数是不同方向,不算重复代码):

| PG 值 | 返回给客户端 |
|---|---|
| `read uncommitted` | `READ-UNCOMMITTED` |
| `read committed` | `READ-COMMITTED` |
| `repeatable read` | `REPEATABLE-READ` |
| `serializable` | `SERIALIZABLE` |

`global.transaction_isolation` **不要**在这里特例化——GLOBAL 作用域本次不实现,让它继续走原路径,保持"未实现"的一致性,不要制造"读得到但设不进去"的假象。

---

## 5. 明确不在本次范围(不要顺手修)

以下都是本次分析中确认存在的独立问题,各自需要单独设计,混进来会让本次改动无法验证:

| # | 问题 | 为什么单列 |
|---|---|---|
| A | 裸 `SET TRANSACTION ISOLATION LEVEL x` 的"仅下一个事务"语义 | PG 无直接对应。可行方案:存一个 pending 值,在 `mys_utility.c:385-409` 已有的 `TRANS_STMT_BEGIN`/`TRANS_STMT_START` 拦截点应用。需要新增会话状态。**⛔ 已裁决不做(2026-08-14)**:用户定下"兼容功能与 openHalo 持平即可"的目标口径,openHalo 上游同样只存自有变量表、不真正生效;裸 `SET TRANSACTION` 保持与上游一致。读路径因步骤 4 已改读真实 GUC,不会展示假值 |
| B | `SET GLOBAL TRANSACTION ISOLATION LEVEL x` | 需要 shmem 全局变量(`globalSystemVars` 已存在)+ 新会话启动时把它读进 `default_transaction_isolation` 的钩子。**只存不用会变成"读得到但不生效"的假象,比不实现更糟**。**⛔ 已裁决不做(2026-08-14)**:同上,与 openHalo 上游一致。`@@global.transaction_isolation` 继续读 MySQL 自有全局变量存储(种子值 `REPEATABLE-READ`),与"GLOBAL 未接线"的状态自洽,已在代码注释中说明 |
| C | **`SHOW VARIABLES` 全部返回空值** | 视图 `mys_informa_schema.empty_session_variables`(`aux_mysql--1.3--1.4.sql:3531`)对所有变量硬编码 `''::varchar(1024) as value`。且 `getSystemVariableValueForShow()` 定义了**从未被任何代码调用**(死代码)。这是整个 `SHOW VARIABLES` 的缺口,不止隔离级别。对应 FUSION_PLAN.md §5.8 里记的"`SHOW VARIABLES LIKE 'version%'` 值为空"那条"次要瑕疵"——实际严重度比当初判断的高 |
| D | MySQL 默认 REPEATABLE READ vs 本集群默认 read committed | ✅ **已解决(2026-08-14,同日,P2-16)**。当时以为是"改共享 GUC,全局二选一"的策略决定——用户指出这个前提本身有问题(会污染 PG/TDS 共用的集群级 GUC)。实际方案:用 `ProtocolRoutine.session_initialize`(MySQL 专属的每连接钩子 `mysql_session_initialize()`)按会话设置,不碰 `postgresql.conf`。TDS 侧顺带实测确认真实 SQL Server 默认是 `READ COMMITTED`,不需要改。详见 FUSION_PLAN.md P2-16 |
| E | `mys_gram.y:2243-2244` 的复制粘贴 bug | `SESSION CHARACTERISTICS AS TRANSACTION transaction_mode_list` 连写两遍。因 `set_rest` 在普通 `SET` 下不可达而从未暴露。改它需要动 bison,收益为零,本次不碰 |

---

## 6. 验证方案

改完后按顺序验证,**每一步都要看到预期输出再往下走**。

### 6.1 构建与部署

改动涉及内核(`mys_uservar.c`)和扩展(`mys_utility.c`),两边都要重建:

```bash
cd /home/hlv/openhalo-update/postgresql_modified_for_babelfish
ninja -C build && ninja -C build install
./inst/bin/pg_ctl -D /tmp/claude-1000/-home-hlv-openhalo-update/04cab886-b08f-4c72-b9f9-64ce334fae62/scratchpad/mysqltest restart
```

> 重启可能超过 30s,用后台执行或调大 timeout,不要误判成失败。

### 6.2 功能验证(MySQL 客户端,端口 3306,用户 test/test)

| 用例 | 修复前 | 修复后应为 |
|---|---|---|
| `SELECT @@transaction_isolation;` | `REPEATABLE-READ`(假值) | `READ-COMMITTED`(真值,与 PG 默认一致) |
| `SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE;` 后再查 | 仍 `REPEATABLE-READ` | `SERIALIZABLE` |
| 同上,在新事务里查 | 仍 `REPEATABLE-READ` | `SERIALIZABLE` |
| `START TRANSACTION ISOLATION LEVEL READ COMMITTED;` | **ERROR 1210** | 成功,无报错 |
| 设成 SERIALIZABLE 后 `psql` 侧查 `SHOW default_transaction_isolation` | 不受影响 | `serializable`(证明真的写进了 PG) |

最后一条是关键的**交叉验证**:证明 MySQL 侧的 SET 真正改变了后端状态,而不是又一个自说自话的模拟层。

### 6.3 回归验证(必须做)

```bash
cd /home/hlv/openhalo-update/postgresql_modified_for_babelfish
PERL5LIB=/home/hlv/perl5/lib/perl5 meson test -C build \
  --suite setup --suite postmaster --suite aux_mysql --suite regress
```

基线 **13/13**,必须保持。`PERL5LIB` 不能省(见 FUSION_PLAN.md P1-6)。

### 6.4 跨方言隔离验证(对应用户的"三种兼容模式互不干扰"原则)

本次改动全部落在 MySQL 专属代码路径(`mysql._` 前缀分发、`mys_get_system_variable`),理论上碰不到 PG/TDS。但仍需实测确认:

- `psql` 连 5432/5433:`SHOW default_transaction_isolation` 行为不变
- TDS 客户端连 1433:随便跑一条查询,确认连接与事务正常

---

## 7. 完成后要更新的文档

1. `FUSION_PLAN.md` §6 的 P2-10 条目:标记已修复,附本文档链接。
2. `FUSION_PLAN.md` §6 新增条目:第 5 节的 A/B/C/D 四项(E 可以并进 C 或单列)。特别是 **C(SHOW VARIABLES 全空)** 严重度高于原先记录,要修正 §5.8 里"次要瑕疵"的措辞。
3. `FUSION_PLAN.md` §9 待提交清单:加入本次改动的两个文件。
